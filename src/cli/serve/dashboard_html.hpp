#ifndef GUFO_SERVER_DASHBOARD_HTML_HPP_
#define GUFO_SERVER_DASHBOARD_HTML_HPP_

#include <cstddef>

namespace gufo::server {

/// The dashboard page, embedded at build time from
/// src/cli/serve/web/dashboard.html (see cmake/embed_file.cmake).
/// kDashboardHtmlSize excludes the trailing NUL.
extern const unsigned char kDashboardHtml[];
extern const unsigned long kDashboardHtmlSize;

}  // namespace gufo::server

#endif  // GUFO_SERVER_DASHBOARD_HTML_HPP_
