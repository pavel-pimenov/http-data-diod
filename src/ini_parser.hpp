#ifndef INI_PARSER_HPP
#define INI_PARSER_HPP

#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Minimal INI parser (no external dependencies). Supports:
//   [section]
//   key = value
// comments starting with ';' or '#', blank lines, '=' inside values.
// Section and key names are kept as-is (trimmed). Lookups are case-insensitive.
namespace IniParser {

struct Section {
  std::string name;
  std::unordered_map<std::string, std::string> keys;
};

inline std::string trim(const std::string &s) {
  const auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return {};
  }
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

// Lower-cases a string for case-insensitive lookups.
inline std::string to_lower(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

// Parses an INI file. Returns true on success (even if the file is empty).
// On parse error (missing file) returns false and fills `error`.
inline bool parse_file(const std::string &path,
                       std::vector<Section> &sections,
                       std::string &error) {
  std::ifstream in(path);
  if (!in.is_open()) {
    error = "cannot open INI file '" + path + "'";
    return false;
  }
  sections.clear();
  std::string line;
  Section current;
  bool have_section = false;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#') {
      continue;
    }
    if (t[0] == '[') {
      const auto close = t.find(']');
      if (close == std::string::npos) {
        error = "malformed section header: " + t;
        return false;
      }
      if (have_section) {
        sections.push_back(std::move(current));
        current = Section{};
      }
      current.name = trim(t.substr(1, close - 1));
      have_section = true;
      continue;
    }
    const auto eq = t.find('=');
    if (eq == std::string::npos) {
      error = "malformed line (no '='): " + t;
      return false;
    }
    if (!have_section) {
      error = "key outside any section: " + t;
      return false;
    }
    const std::string key = to_lower(trim(t.substr(0, eq)));
    const std::string value = trim(t.substr(eq + 1));
    current.keys[key] = value;
  }
  if (have_section) {
    sections.push_back(std::move(current));
  }
  return true;
}

// Finds a section by name (case-insensitive). Returns nullptr if absent.
inline const Section *find_section(const std::vector<Section> &sections,
                                   const std::string &name) {
  const std::string lower = to_lower(name);
  for (const auto &s : sections) {
    if (to_lower(s.name) == lower) {
      return &s;
    }
  }
  return nullptr;
}

// Gets a key value from a section (case-insensitive). Returns std::nullopt if
// the key is missing.
inline std::optional<std::string> get_key(const Section &section,
                                          const std::string &key) {
  const std::string lower = to_lower(key);
  const auto it = section.keys.find(lower);
  if (it == section.keys.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace IniParser

#endif  // INI_PARSER_HPP
