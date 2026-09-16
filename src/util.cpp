#include "util.hpp"

#include <cctype>
#include <cstdlib>

namespace aes67_srt {

std::string trim(const std::string& text) {
  size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::string to_lower(const std::string& text) {
  std::string result = text;
  for (char& character : result) {
    character =
        static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return result;
}

std::vector<std::string> split(const std::string& text, char separator) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (true) {
    const size_t position = text.find(separator, start);
    if (position == std::string::npos) {
      fields.push_back(text.substr(start));
      return fields;
    }
    fields.push_back(text.substr(start, position - start));
    start = position + 1;
  }
}

bool split_host_port(const std::string& text, std::string* host, int* port) {
  const std::vector<std::string> fields = split(trim(text), ':');
  if (fields.size() != 2) {
    return false;
  }
  const std::string name = trim(fields[0]);
  const std::string number = trim(fields[1]);
  if (name.empty() || number.empty()) {
    return false;
  }
  for (char character : number) {
    if (!std::isdigit(static_cast<unsigned char>(character))) {
      return false;
    }
  }
  const long value = std::strtol(number.c_str(), nullptr, 10);
  if (value < 1 || value > 65535) {
    return false;
  }
  *host = name;
  *port = static_cast<int>(value);
  return true;
}

}  // namespace aes67_srt
