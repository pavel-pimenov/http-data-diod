#ifndef REQUEST_DATA_PREPARER_HPP
#define REQUEST_DATA_PREPARER_HPP

#include "httplib/httplib.h"
#include <nlohmann/json.hpp>
#include <string>

// Prepare request data for backend
nlohmann::json prepare_request_data(const std::string &request_id,
                                    const std::string &method,
                                    const std::string &path,
                                    const std::string &body,
                                    const httplib::Request &req,
                                    const std::string &traceparent_for_backend);

#endif // REQUEST_DATA_PREPARER_HPP