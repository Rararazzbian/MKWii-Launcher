// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/MemInspect/WebServer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/Socket.hpp>
#include <SFML/Network/SocketSelector.hpp>
#include <SFML/Network/TcpListener.hpp>
#include <SFML/Network/TcpSocket.hpp>
#include <SFML/System/Time.hpp>
#include <fmt/format.h>
#include <picojson.h>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Core/MemInspect/Dashboard.h"
#include "Core/MemInspect/Inspector.h"
#include "Core/MemInspect/Items.h"
#include "Core/MemInspect/Registry.h"
#include "Core/MemInspect/States.h"

namespace MemInspect::Web
{
namespace
{
// A request body is a handful of small fields. Anything larger is not something
// this server has a route for, and refusing early keeps a stuck connection from
// growing without bound.
constexpr std::size_t MAX_REQUEST_BYTES = 64 * 1024;
// How long the accept loop blocks waiting for socket activity. Short enough
// that a new snapshot reaches an SSE client promptly, long enough that an idle
// server costs nothing.
constexpr int SELECT_INTERVAL_MS = 15;

// ---------------------------------------------------------------------------
// JSON output.
//
// Hand-written rather than built through picojson because the state payload is
// emitted many times a second and there is no reason to materialise a tree of
// std::map nodes just to serialise it once. picojson is still used for PARSING
// request bodies, where the tree is the point.

class JsonWriter
{
public:
  void BeginObject()
  {
    Comma();
    m_out += '{';
    m_need_comma = false;
  }
  void EndObject()
  {
    m_out += '}';
    m_need_comma = true;
  }
  void BeginArray()
  {
    Comma();
    m_out += '[';
    m_need_comma = false;
  }
  void EndArray()
  {
    m_out += ']';
    m_need_comma = true;
  }

  void Key(std::string_view key)
  {
    Comma();
    AppendString(key);
    m_out += ':';
    // A key is followed by exactly one value, which must not be preceded by a
    // comma of its own.
    m_need_comma = false;
  }

  void Null()
  {
    Comma();
    m_out += "null";
    m_need_comma = true;
  }
  void Bool(bool value)
  {
    Comma();
    m_out += value ? "true" : "false";
    m_need_comma = true;
  }
  void Int(s64 value)
  {
    Comma();
    m_out += std::to_string(value);
    m_need_comma = true;
  }
  void Real(double value)
  {
    // NaN and infinity are not JSON. A position read through a chain that has
    // drifted can produce either, so they become null rather than syntax the
    // browser would refuse to parse.
    if (!std::isfinite(value))
    {
      Null();
      return;
    }
    Comma();
    m_out += fmt::format("{}", value);
    m_need_comma = true;
  }
  void String(std::string_view value)
  {
    Comma();
    AppendString(value);
    m_need_comma = true;
  }

  void Field(std::string_view key, bool value)
  {
    Key(key);
    Bool(value);
  }
  void Field(std::string_view key, s64 value)
  {
    Key(key);
    Int(value);
  }
  void Field(std::string_view key, double value)
  {
    Key(key);
    Real(value);
  }
  void Field(std::string_view key, std::string_view value)
  {
    Key(key);
    String(value);
  }
  // Without this overload a string literal would bind to Field(key, bool) -
  // pointer-to-bool is a standard conversion and beats the one to string_view -
  // and every literal field would silently serialise as `true`.
  void Field(std::string_view key, const char* value)
  {
    Key(key);
    String(value);
  }

  const std::string& Result() const { return m_out; }

private:
  void Comma()
  {
    if (m_need_comma)
      m_out += ',';
  }

  void AppendString(std::string_view value)
  {
    m_out += '"';
    for (const char c : value)
    {
      switch (c)
      {
      case '"':
        m_out += "\\\"";
        break;
      case '\\':
        m_out += "\\\\";
        break;
      case '\n':
        m_out += "\\n";
        break;
      case '\r':
        m_out += "\\r";
        break;
      case '\t':
        m_out += "\\t";
        break;
      default:
        // Everything below space must be escaped; UTF-8 continuation bytes are
        // passed through untouched, which is what the browser wants.
        if (static_cast<unsigned char>(c) < 0x20)
          m_out += fmt::format("\\u{:04x}", static_cast<unsigned char>(c));
        else
          m_out += c;
        break;
      }
    }
    m_out += '"';
  }

  std::string m_out;
  bool m_need_comma = false;
};

void WriteReading(JsonWriter& json, const Reading& reading)
{
  if (!reading.ok)
  {
    json.Null();
    return;
  }
  switch (reading.kind)
  {
  case Kind::Bool:
    json.Bool(reading.integer != 0);
    break;
  case Kind::F32:
  case Kind::F64:
    json.Real(reading.real);
    break;
  case Kind::Str16:
    json.String(reading.text);
    break;
  default:
    json.Int(reading.integer);
    break;
  }
}

std::string StateJson()
{
  const Snapshot snapshot = Inspector::GetInstance().GetSnapshot();

  JsonWriter json;
  json.BeginObject();
  json.Field("hooked", snapshot.hooked);
  json.Key("game");
  if (snapshot.game.empty())
    json.Null();
  else
    json.String(snapshot.game);
  json.Field("hz", snapshot.hz);
  json.Field("ts", static_cast<s64>(snapshot.timestamp_ms));
  json.Field("seq", static_cast<s64>(snapshot.sequence));

  // The resolved address travels alongside every value so a broken pointer
  // chain is visible on the page rather than silently reading as zero.
  json.Key("values");
  json.BeginObject();
  for (const WatchValue& value : snapshot.values)
  {
    json.Key(value.key);
    json.BeginObject();
    json.Key("raw");
    WriteReading(json, value.reading);
    json.Key("addr");
    if (value.resolved)
      json.Int(value.address);
    else
      json.Null();
    json.EndObject();
  }
  json.EndObject();

  json.Key("derived");
  json.BeginObject();
  for (const auto& [name, value] : snapshot.derived)
  {
    json.Key(name);
    if (value)
      json.Bool(*value);
    else
      json.Null();
  }
  json.EndObject();

  json.Key("racers");
  json.BeginArray();
  for (const RacerIdentity& racer : snapshot.racers)
  {
    json.BeginObject();
    json.Field("index", static_cast<s64>(racer.index));
    json.Field("name", racer.name);
    json.Field("source", racer.source);
    json.Field("local", racer.local);
    json.EndObject();
  }
  json.EndArray();

  json.Key("states");
  json.BeginArray();
  for (const RacerState& state : snapshot.states)
  {
    json.BeginObject();
    json.Field("index", static_cast<s64>(state.index));
    json.Key("active");
    json.BeginArray();
    for (const std::string& name : state.active)
      json.String(name);
    json.EndArray();
    json.Field("star", state.star);
    json.Field("shocked", state.shocked);
    json.Field("mega", state.mega);
    json.Field("crushed", state.crushed);
    json.Field("bullet", state.bullet);
    json.Field("inked", state.inked);
    json.Field("has_tc", state.has_tc);
    json.Field("stopped", state.stopped);
    json.Field("vanished", state.vanished);
    json.Field("cpu", state.cpu);
    json.Field("local", state.local);
    json.Field("real_local", state.real_local);
    json.Field("remote", state.remote);
    json.Field("star_timer", static_cast<s64>(state.star_timer));
    json.Field("shock_timer", static_cast<s64>(state.shock_timer));
    json.Field("crush_timer", static_cast<s64>(state.crush_timer));
    json.Field("mega_timer", static_cast<s64>(state.mega_timer));
    json.Field("ink_timer", static_cast<s64>(state.ink_timer));
    json.Field("consistent", state.consistent);
    json.EndObject();
  }
  json.EndArray();

  json.Key("items");
  json.BeginArray();
  for (const ItemSlot& item : snapshot.items)
  {
    json.BeginObject();
    json.Field("index", static_cast<s64>(item.index));
    json.Field("id", static_cast<s64>(item.id));
    json.Field("count", static_cast<s64>(item.count));
    json.Field("name", item.name);
    json.Field("empty", item.empty);
    json.EndObject();
  }
  json.EndArray();

  json.EndObject();
  return json.Result();
}

std::string MetaJson()
{
  JsonWriter json;
  json.BeginObject();
  json.Field("game_id", GAME_ID);
  json.Field("game_note", GAME_NOTE);

  json.Key("watches");
  json.BeginArray();
  for (const Watch& watch : Inspector::GetInstance().GetWatches())
  {
    json.BeginObject();
    json.Field("key", watch.key);
    json.Field("label", watch.label);
    json.Field("group", watch.group);
    json.Field("addr", static_cast<s64>(watch.address));
    json.Field("kind", KindName(watch.kind, watch.chars));
    json.Field("note", watch.note);
    json.Field("fmt", watch.hex ? "hex" : "dec");
    json.Field("chain", !watch.offsets.empty());
    json.Field("user", watch.user);
    json.Key("enum");
    json.BeginObject();
    for (const auto& [value, label] : watch.enum_labels)
    {
      json.Key(std::to_string(value));
      json.String(label);
    }
    json.EndObject();
    json.EndObject();
  }
  json.EndArray();

  json.Key("derived");
  json.BeginArray();
  for (const DerivedPredicate& predicate : DerivedPredicates())
  {
    json.BeginObject();
    json.Field("name", predicate.name);
    json.Field("desc", predicate.description);
    json.EndObject();
  }
  json.EndArray();

  // The types the add-a-watch form offers. str16 is deliberately absent: it
  // needs a length, and the form has nowhere to ask for one.
  json.Key("kinds");
  json.BeginArray();
  for (const char* kind : {"bool", "f32", "f64", "ptr", "s16", "s32", "s8", "u16", "u32", "u8"})
    json.String(kind);
  json.EndArray();

  // Everything the write endpoints will accept, so the page can build its
  // buttons from the same tables the writes validate against.
  json.Key("items");
  json.BeginArray();
  for (const auto& [id, label] : ItemLabels())
  {
    json.BeginObject();
    json.Field("id", id);
    json.Field("name", label);
    json.Field("risk", ItemRisk(static_cast<int>(id)));
    json.EndObject();
  }
  json.EndArray();

  json.Key("states");
  json.BeginArray();
  for (const std::string& name : SettableStates())
    json.String(name);
  json.EndArray();

  json.EndObject();
  return json.Result();
}

std::string ErrorJson(std::string_view message)
{
  JsonWriter json;
  json.BeginObject();
  json.Field("error", message);
  json.EndObject();
  return json.Result();
}

// ---------------------------------------------------------------------------
// HTTP.

struct Request
{
  std::string method;
  std::string path;
  std::string host;
  std::string body;
};

struct Response
{
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  // An SSE response is a header now and an unbounded stream afterwards, so the
  // connection is kept rather than closed once the body has gone out.
  bool stream = false;
};

std::string_view StatusText(int status)
{
  switch (status)
  {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 413:
    return "Payload Too Large";
  default:
    return "Error";
  }
}

std::string Header(const Response& response)
{
  std::string out = fmt::format("HTTP/1.1 {} {}\r\n", response.status, StatusText(response.status));
  out += fmt::format("Content-Type: {}\r\n", response.content_type);
  // The dashboard is live data; a cached copy is never the right answer.
  out += "Cache-Control: no-cache, no-store\r\n";
  if (response.stream)
  {
    out += "Connection: keep-alive\r\n";
    // Without this a reverse proxy would buffer the stream into uselessness.
    out += "X-Accel-Buffering: no\r\n\r\n";
  }
  else
  {
    out += fmt::format("Content-Length: {}\r\n", response.body.size());
    out += "Connection: close\r\n\r\n";
  }
  return out;
}

// The Host header a request must carry. A page on any other origin can still
// make the browser send a request here, so this is what stops a random website
// from driving the write endpoints against a running game.
bool HostIsLoopback(std::string_view host, u16 port)
{
  const std::string expected_v4 = fmt::format("127.0.0.1:{}", port);
  const std::string expected_name = fmt::format("localhost:{}", port);
  const std::string expected_v6 = fmt::format("[::1]:{}", port);
  return host == expected_v4 || host == expected_name || host == expected_v6;
}

std::optional<Request> ParseRequest(const std::string& raw, std::size_t* consumed)
{
  const std::size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos)
    return std::nullopt;

  const std::string_view head(raw.data(), header_end);
  const std::size_t line_end = head.find("\r\n");
  const std::string_view request_line = head.substr(0, line_end);

  Request request;
  const std::size_t first_space = request_line.find(' ');
  const std::size_t second_space =
      first_space == std::string_view::npos ? std::string_view::npos :
                                              request_line.find(' ', first_space + 1);
  if (first_space == std::string_view::npos || second_space == std::string_view::npos)
    return std::nullopt;
  request.method = request_line.substr(0, first_space);
  request.path = request_line.substr(first_space + 1, second_space - first_space - 1);

  std::size_t content_length = 0;
  std::size_t cursor = line_end == std::string_view::npos ? head.size() : line_end + 2;
  while (cursor < head.size())
  {
    const std::size_t next = head.find("\r\n", cursor);
    const std::string_view line =
        head.substr(cursor, next == std::string_view::npos ? std::string_view::npos : next - cursor);
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos)
    {
      std::string name(line.substr(0, colon));
      std::ranges::transform(name, name.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      });
      const std::string_view value = StripWhitespace(line.substr(colon + 1));
      if (name == "content-length")
      {
        u32 parsed = 0;
        if (TryParse(std::string(value), &parsed, 10))
          content_length = parsed;
      }
      else if (name == "host")
      {
        request.host = value;
      }
    }
    if (next == std::string_view::npos)
      break;
    cursor = next + 2;
  }

  const std::size_t body_start = header_end + 4;
  if (raw.size() < body_start + content_length)
    return std::nullopt;  // body still arriving

  request.body = raw.substr(body_start, content_length);
  *consumed = body_start + content_length;
  return request;
}

std::optional<int> JsonInt(const picojson::object& object, const std::string& key)
{
  const auto found = object.find(key);
  if (found == object.end() || !found->second.is<double>())
    return std::nullopt;
  return static_cast<int>(found->second.get<double>());
}

std::string JsonString(const picojson::object& object, const std::string& key)
{
  const auto found = object.find(key);
  if (found == object.end() || !found->second.is<std::string>())
    return {};
  return found->second.get<std::string>();
}

bool JsonBool(const picojson::object& object, const std::string& key, bool fallback)
{
  const auto found = object.find(key);
  if (found == object.end() || !found->second.is<bool>())
    return fallback;
  return found->second.get<bool>();
}

Response HandleAddWatch(const Request& request)
{
  picojson::value parsed;
  const std::string error = picojson::parse(parsed, request.body);
  if (!error.empty() || !parsed.is<picojson::object>())
    return {.status = 400, .body = ErrorJson("body must be a JSON object")};
  const picojson::object& object = parsed.get<picojson::object>();

  // The form sends hex without a prefix, which is how every address in the
  // documentation is written.
  std::string text = JsonString(object, "addr");
  if (text.starts_with("0x") || text.starts_with("0X"))
    text = text.substr(2);
  u32 address = 0;
  if (text.empty() || !TryParse("0x" + text, &address))
    return {.status = 400, .body = ErrorJson("address must be hex, e.g. 80444804")};

  std::string kind = JsonString(object, "kind");
  if (kind.empty())
    kind = "u32";

  std::string message;
  if (!Inspector::GetInstance().AddUserWatch(address, kind, JsonString(object, "label"),
                                             JsonString(object, "fmt") == "hex", &message))
  {
    return {.status = 400, .body = ErrorJson(message)};
  }

  JsonWriter json;
  json.BeginObject();
  json.Field("ok", true);
  json.EndObject();
  return {.body = json.Result()};
}

Response HandleGiveItem(const Request& request)
{
  picojson::value parsed;
  const std::string error = picojson::parse(parsed, request.body);
  if (!error.empty() || !parsed.is<picojson::object>())
    return {.status = 400, .body = ErrorJson("body must be a JSON object")};
  const picojson::object& object = parsed.get<picojson::object>();

  const auto racer = JsonInt(object, "racer");
  if (!racer || *racer < 0 || *racer >= MAX_RACERS)
    return {.status = 400, .body = ErrorJson("racer must be 0-11")};

  // Either a numeric id or a name/alias, so the page can send whichever it has.
  std::optional<int> id = JsonInt(object, "id");
  if (!id)
    id = LookupItem(JsonString(object, "item"));
  if (!id)
    return {.status = 400, .body = ErrorJson("unknown item")};

  const bool force = JsonBool(object, "force", false);
  if (!force && (*id < 0 || *id > ITEM_MAX_ID))
  {
    return {.status = 400,
            .body = ErrorJson(fmt::format("item id {} is outside 0-{}. {} is confirmed to crash "
                                          "the game and anything above it reads past the same "
                                          "table end.",
                                          *id, ITEM_MAX_ID, ITEM_CRASH_ID))};
  }

  Inspector::GetInstance().QueueGiveItem(*racer, *id, JsonInt(object, "count"), force);

  JsonWriter json;
  json.BeginObject();
  json.Field("ok", true);
  json.Field("id", static_cast<s64>(*id));
  json.Field("name", ItemName(*id));
  json.EndObject();
  return {.body = json.Result()};
}

Response HandleSetState(const Request& request)
{
  picojson::value parsed;
  const std::string error = picojson::parse(parsed, request.body);
  if (!error.empty() || !parsed.is<picojson::object>())
    return {.status = 400, .body = ErrorJson("body must be a JSON object")};
  const picojson::object& object = parsed.get<picojson::object>();

  const auto racer = JsonInt(object, "racer");
  if (!racer || *racer < 0 || *racer >= MAX_RACERS)
    return {.status = 400, .body = ErrorJson("racer must be 0-11")};

  if (JsonBool(object, "clear", false))
  {
    Inspector::GetInstance().QueueClearStates(*racer);
    JsonWriter json;
    json.BeginObject();
    json.Field("ok", true);
    json.EndObject();
    return {.body = json.Result()};
  }

  const std::string name = JsonString(object, "state");
  const auto& settable = SettableStates();
  if (std::ranges::find(settable, name) == settable.end())
    return {.status = 400, .body = ErrorJson("unknown state " + name)};

  Inspector::GetInstance().QueueSetState(*racer, name, JsonInt(object, "frames"),
                                         JsonBool(object, "on", true));

  JsonWriter json;
  json.BeginObject();
  json.Field("ok", true);
  // Setting a state other than star sets the flag and the timer but changes
  // nothing on screen, because the game does the visible part in its activation
  // routine rather than deriving it from the timer. Say so rather than letting
  // the page report a success that looks like a failure.
  json.Field("effective", name == "star");
  json.EndObject();
  return {.body = json.Result()};
}

Response Route(const Request& request, u16 port)
{
  if (!HostIsLoopback(request.host, port))
  {
    return {.status = 403,
            .body = ErrorJson("this server only answers requests addressed to localhost")};
  }

  const std::string_view path = request.path;

  if (request.method == "GET" && (path == "/" || path == "/index.html"))
    return {.content_type = "text/html; charset=utf-8", .body = std::string(DashboardHtml())};

  if (request.method == "GET" && path == "/api/meta")
    return {.body = MetaJson()};

  if (request.method == "GET" && path == "/api/state")
    return {.body = StateJson()};

  if (request.method == "GET" && path == "/events")
    return {.content_type = "text/event-stream", .stream = true};

  if (path == "/api/watch")
  {
    if (request.method == "POST")
      return HandleAddWatch(request);
    return {.status = 405, .body = ErrorJson("use POST")};
  }

  if (path.starts_with("/api/watch/"))
  {
    if (request.method != "DELETE")
      return {.status = 405, .body = ErrorJson("use DELETE")};
    const std::string key(path.substr(std::string_view("/api/watch/").size()));
    Inspector::GetInstance().RemoveUserWatch(key);
    JsonWriter json;
    json.BeginObject();
    json.Field("ok", true);
    json.EndObject();
    return {.body = json.Result()};
  }

  if (path == "/api/item")
  {
    if (request.method == "POST")
      return HandleGiveItem(request);
    return {.status = 405, .body = ErrorJson("use POST")};
  }

  if (path == "/api/state")
  {
    if (request.method == "POST")
      return HandleSetState(request);
    return {.status = 405, .body = ErrorJson("use POST")};
  }

  return {.status = 404, .body = ErrorJson("no such endpoint")};
}

// ---------------------------------------------------------------------------
// The server.

struct Connection
{
  std::unique_ptr<sf::TcpSocket> socket;
  std::string incoming;
  std::string outgoing;
  bool streaming = false;
  // The last snapshot sequence pushed to this client. `sent_any` is separate
  // from a zero sequence because with no game running the sequence IS zero, and
  // the page still needs that first "nothing is hooked" payload to render at
  // all.
  bool sent_any = false;
  u64 sent_sequence = 0;
  bool close_when_drained = false;
};

class Server
{
public:
  bool Start(u16 port);
  void Stop();
  bool IsRunning() const { return m_running.load(); }
  u16 Port() const { return m_port.load(); }

private:
  void Run();
  void Accept();
  void Receive(Connection& connection);
  void PushEvents();
  // Returns false when the connection is finished with.
  bool Flush(Connection& connection);

  sf::TcpListener m_listener;
  std::vector<Connection> m_connections;
  std::thread m_thread;
  std::atomic<bool> m_running{false};
  std::atomic<u16> m_port{0};
};

bool Server::Start(u16 port)
{
  if (m_running.load())
    return false;

  // Loopback only. See the header: this hands out live reads of a running game
  // and accepts writes back into it.
  if (m_listener.listen(port, sf::IpAddress::LocalHost) != sf::Socket::Status::Done)
  {
    ERROR_LOG_FMT(COMMON, "Memory inspector: could not listen on 127.0.0.1:{}", port);
    return false;
  }
  m_listener.setBlocking(false);
  m_port.store(port);
  m_running.store(true);
  m_thread = std::thread(&Server::Run, this);
  INFO_LOG_FMT(COMMON, "Memory inspector: http://127.0.0.1:{}", port);
  return true;
}

void Server::Stop()
{
  if (!m_running.exchange(false))
    return;
  if (m_thread.joinable())
    m_thread.join();
  m_connections.clear();
  m_listener.close();
  m_port.store(0);
}

void Server::Run()
{
  Common::SetCurrentThreadName("MemInspect Web");

  while (m_running.load())
  {
    sf::SocketSelector selector;
    selector.add(m_listener);
    for (const Connection& connection : m_connections)
    {
      if (connection.socket)
        selector.add(*connection.socket);
    }

    if (selector.wait(sf::milliseconds(SELECT_INTERVAL_MS)))
    {
      if (selector.isReady(m_listener))
        Accept();
      for (Connection& connection : m_connections)
      {
        if (connection.socket && selector.isReady(*connection.socket))
          Receive(connection);
      }
    }

    PushEvents();

    std::erase_if(m_connections, [this](Connection& connection) { return !Flush(connection); });
  }
}

void Server::Accept()
{
  auto socket = std::make_unique<sf::TcpSocket>();
  if (m_listener.accept(*socket) != sf::Socket::Status::Done)
    return;
  socket->setBlocking(false);
  m_connections.push_back({.socket = std::move(socket)});
}

void Server::Receive(Connection& connection)
{
  std::array<char, 4096> buffer{};
  std::size_t received = 0;
  const sf::Socket::Status status =
      connection.socket->receive(buffer.data(), buffer.size(), received);

  if (status == sf::Socket::Status::Disconnected || status == sf::Socket::Status::Error)
  {
    connection.socket.reset();
    return;
  }
  if (status != sf::Socket::Status::Done || received == 0)
    return;

  connection.incoming.append(buffer.data(), received);
  if (connection.incoming.size() > MAX_REQUEST_BYTES)
  {
    const Response response = {.status = 413, .body = ErrorJson("request too large")};
    connection.outgoing += Header(response) + response.body;
    connection.close_when_drained = true;
    connection.incoming.clear();
    return;
  }

  // A streaming client has nothing more to say; anything it sends is ignored
  // rather than parsed as a second request on the same connection.
  if (connection.streaming)
  {
    connection.incoming.clear();
    return;
  }

  std::size_t consumed = 0;
  const auto request = ParseRequest(connection.incoming, &consumed);
  if (!request)
    return;
  connection.incoming.erase(0, consumed);

  const Response response = Route(*request, m_port.load());
  connection.outgoing += Header(response);
  connection.outgoing += response.body;

  if (response.stream)
  {
    connection.streaming = true;
    // Send the current snapshot straight away rather than making the page wait
    // for the next one - which, with no game running, would never arrive.
    connection.sent_any = false;
  }
  else
  {
    connection.close_when_drained = true;
  }
}

void Server::PushEvents()
{
  const u64 sequence = Inspector::GetInstance().GetSequence();

  std::string payload;
  for (Connection& connection : m_connections)
  {
    if (!connection.streaming || !connection.socket)
      continue;
    if (connection.sent_any && connection.sent_sequence == sequence)
      continue;
    if (payload.empty())
      payload = "data: " + StateJson() + "\n\n";
    connection.outgoing += payload;
    connection.sent_any = true;
    connection.sent_sequence = sequence;
  }
}

bool Server::Flush(Connection& connection)
{
  if (!connection.socket)
    return false;

  while (!connection.outgoing.empty())
  {
    std::size_t sent = 0;
    const sf::Socket::Status status =
        connection.socket->send(connection.outgoing.data(), connection.outgoing.size(), sent);
    connection.outgoing.erase(0, sent);

    if (status == sf::Socket::Status::Done)
      continue;
    if (status == sf::Socket::Status::Partial || status == sf::Socket::Status::NotReady)
      return true;  // the rest goes out on a later pass
    return false;   // disconnected or errored
  }

  return !connection.close_when_drained;
}

Server& GetServer()
{
  static Server server;
  return server;
}
}  // namespace

bool Start(u16 port)
{
  return GetServer().Start(port);
}

void Stop()
{
  GetServer().Stop();
}

bool IsRunning()
{
  return GetServer().IsRunning();
}

u16 GetPort()
{
  return GetServer().Port();
}

std::string GetURL()
{
  const u16 port = GetPort();
  if (port == 0)
    return {};
  return fmt::format("http://127.0.0.1:{}", port);
}
}  // namespace MemInspect::Web
