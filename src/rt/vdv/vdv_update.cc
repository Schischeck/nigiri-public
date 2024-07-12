#include "nigiri/rt/vdv/vdv_update.h"

#include <sstream>
#include <string>
#include <string_view>

#include "pugixml.hpp"

#include "utl/enumerate.h"
#include "utl/parser/arg_parser.h"
#include "utl/to_vec.h"
#include "utl/verify.h"

#include "nigiri/common/linear_lower_bound.h"
#include "nigiri/logging.h"
#include "nigiri/rt/run.h"
#include "nigiri/rt/vdv/vdv_resolve_run.h"
#include "nigiri/rt/vdv/vdv_run.h"
#include "nigiri/rt/vdv/vdv_xml.h"
#include "nigiri/timetable.h"

namespace nigiri::rt::vdv {

unixtime_t parse_time(std::string_view str) {
  unixtime_t parsed;
  auto ss = std::stringstream{str};
  ss >> date::parse("%FT%T", parsed);
  return parsed;
}

std::optional<unixtime_t> get_opt_time(pugi::xml_node const& node,
                                       char const* str) {
  auto const xpath = node.select_node(str);
  return xpath ? std::optional{parse_time(xpath.node().child_value())}
               : std::nullopt;
}

std::optional<bool> get_opt_bool(
    pugi::xml_node const& node,
    char const* key,
    std::optional<bool> default_to = std::nullopt) {
  auto const xpath = node.select_node(key);
  return xpath ? utl::parse<bool>(xpath.node().child_value()) : default_to;
}

pugi::xml_node get(pugi::xml_node const& node, char const* str) {
  auto const xpath = node.select_node(str);
  utl::verify(xpath, "required xml node not found: {}", str);
  return xpath.node();
}

struct vdv_stop {
  explicit vdv_stop(pugi::xml_node const n)
      : id_{get(n, "HaltID").child_value()},
        dep_{get_opt_time(n, "Abfahrtszeit")},
        arr_{get_opt_time(n, "Ankunftszeit")},
        rt_dep_{get_opt_time(n, "IstAnkunftPrognose")},
        rt_arr_{get_opt_time(n, "IstAbfahrtPrognose")},
        is_additional_{get_opt_bool(n, "Zusatzhalt", false)} {}

  std::pair<unixtime_t, event_type> get_event() const {
    if (dep_.has_value()) {
      return {*dep_, event_type::kDep};
    } else if (arr_.has_value()) {
      return {*arr_, event_type::kArr};
    } else {
      throw utl::fail("no event found (stop={})", id_);
    }
  }

  std::string_view id_;
  std::optional<unixtime_t> dep_, arr_, rt_dep_, rt_arr_;
  bool is_additional_;
};

std::optional<rt::run> get_run(timetable const& tt,
                               rt_timetable const& rtt,
                               source_idx_t const src,
                               pugi::xml_node const n) {
  auto const stops = utl::to_vec(n.select_nodes("IstHalt"),
                                 [](auto&& n) { return vdv_stop{n.node()}; });
  auto const first_it =
      utl::find_if(stops, [](auto&& s) { return !s.is_additional_; });
  if (first_it == end(stops)) {
    return std::nullopt;
  }

  auto const& first_stop = *first_it;
  auto const l = tt.locations_.get({first_stop.id_, src}).l_;

  for (auto const r : tt.location_routes_[l]) {
    auto const location_seq = tt.route_location_seq_[r];
    for (auto const [stop_idx, s] : utl::enumerate(location_seq)) {
      if (stop{s}.location_idx() != l) {
        continue;
      }

      auto const [t, ev_type] = first_stop.get_event();
      auto const [day_idx, mam] = tt.day_idx_mam(t);
      auto const event_times =
          tt.event_times_at_stop(r, stop_idx, event_type::kDep);
      auto const it = utl::find_if(event_times, [&](delta const ev_time) {
        return ev_time.mam() == mam.count();
      });
      if (it == end(event_times)) {
        continue;
      }

      auto const ev_day_offset = it->days();
      auto const start_day =
          static_cast<std::size_t>(to_idx(day_idx) - ev_day_offset);
      auto const tr =
          tt.route_transport_ranges_[r][std::distance(begin(event_times), it)];
      if (tt.bitfields_[tt.transport_traffic_days_[tr]].test(start_day)) {
        return rt::run{transport{tr, day_idx_t{start_day}},
                       {0U, static_cast<stop_idx_t>(location_seq.size())}};
      }
    }
  }

  return std::nullopt;
}

void process_vdv_run(timetable const& tt,
                     rt_timetable& rtt,
                     source_idx_t const src,
                     pugi::xml_node const& r) {

  for (auto const stop : r.select_nodes("IstHalt")) {
  }
}

statistics vdv_update(timetable const& tt,
                      rt_timetable& rtt,
                      source_idx_t const src,
                      pugi::xml_document const& doc) {
  auto stats = statistics{};
  for (auto const& r :
       doc.select_nodes("DatenAbrufenAntwort/AUSNachricht/IstFahrt")) {
    if (get_opt_bool(r.node(), "Zusatzfahrt", false).value()) {
      ++stats.unsupported_additional_;
      continue;
    } else if (get_opt_bool(r.node(), "FaelltAus", false).value()) {
      ++stats.unsupported_cancelled_;
      continue;
    }

    process_vdv_run(tt, rtt, src, r.node());
  }
  return stats;
}

}  // namespace nigiri::rt::vdv