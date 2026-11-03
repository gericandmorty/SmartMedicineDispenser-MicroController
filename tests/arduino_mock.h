// Host-only Arduino shims. These simulate logic, not electrical behavior.
#pragma once
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <type_traits>
#include <vector>
using byte = uint8_t;
class String {
 public:
  std::string value;
  String() = default;
  String(const char* s): value(s ? s : "") {}
  String(std::string s): value(s) {}
  String(char c): value(1, c) {}
  template<class T, typename = std::enable_if_t<std::is_arithmetic<T>::value>>
  String(T n): value(std::to_string(n)) {}
  unsigned int length() const { return value.size(); }
  const char* c_str() const { return value.c_str(); }
  char operator[](size_t n) const { return n < value.size() ? value[n] : 0; }
  char charAt(size_t n) const { return (*this)[n]; }
  int indexOf(const String& s, int start = 0) const {
    auto n = value.find(s.value, start); return n == std::string::npos ? -1 : int(n);
  }
  String substring(int start, int end = -1) const {
    if (start >= int(value.size())) return "";
    return value.substr(start, end < 0 ? std::string::npos : end - start);
  }
  void trim() {
    auto a = value.find_first_not_of(" \r\n\t");
    if (a == std::string::npos) { value.clear(); return; }
    value = value.substr(a, value.find_last_not_of(" \r\n\t") - a + 1);
  }
  void toUpperCase() { for (char& c : value) c = std::toupper(c); }
  bool equalsIgnoreCase(const String& other) const {
    String a = *this, b = other; a.toUpperCase(); b.toUpperCase(); return a == b;
  }
  bool startsWith(const String& other) const { return value.rfind(other.value, 0) == 0; }
  long toInt() const { return std::strtol(c_str(), nullptr, 10); }
  String& operator+=(const String& s) { value += s.value; return *this; }
  friend String operator+(String a, const String& b) { return a += b; }
  friend bool operator==(const String& a, const String& b) { return a.value == b.value; }
  friend bool operator!=(const String& a, const String& b) { return !(a == b); }
};
constexpr int LOW = 0, HIGH = 1, OUTPUT = 1, INPUT_PULLUP = 2;
unsigned long fakeMillis = 0;
int pinState[64] = {};
unsigned long millis() { return fakeMillis; }
void delay(unsigned long ms) { fakeMillis += ms; }
void digitalWrite(int pin, int state) { pinState[pin] = state; }
int digitalRead(int pin) { return pinState[pin]; }
void pinMode(int, int) {}
bool isDigit(char c) { return std::isdigit(c); }
struct SerialMock {
  std::vector<String> lines;
  String input;
  void begin(int) {}
  operator bool() const { return true; }
  void setTimeout(int) {}
  int available() { return input.length(); }
  String readStringUntil(char) { String s = input; input = ""; return s; }
  template<class T> void print(T) {}
  void println() {}
  template<class T> void println(T t) { lines.push_back(String(t)); }
  template<class... T> void printf(const char*, T...) {}
} Serial;
struct WireMock {
  void begin() {} void end() {} void setClock(int) {}
  void beginTransmission(int) {} int endTransmission() { return 0; }
} Wire;
class LiquidCrystal_I2C {
 public:
  LiquidCrystal_I2C(int, int, int) {}
  void init() {} void backlight() {} void clear() {}
  void setCursor(int, int) {} void print(const String&) {}
};
struct ServerMock {
  std::map<std::string, String> args;
  bool hasArg(const char* key) { return args.count(key); }
  String arg(const char* key) { return args[key]; }
  template<class... T> void send(T...) {}
  template<class... T> void sendHeader(T...) {}
};
