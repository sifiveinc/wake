#pragma once

#include <string>

struct Database;

// Serve the read-only artifact triage UI until interrupted. Returns an exit status.
int serve_ui(Database &db, const std::string &address, const std::string &port);
