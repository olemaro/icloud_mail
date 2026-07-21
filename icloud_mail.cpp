// FastMail Client — native Win32 C++, zero external deps
// Compile MSVC:  cl /O2 /EHsc /MT /DUNICODE /D_UNICODE icloud_mail.cpp
// Compile MinGW: g++ -std=c++17 -O2 -DUNICODE -D_UNICODE -mwindows -static icloud_mail.cpp -o icloud_mail.exe -lws2_32 -lsecur32 -lcomctl32 -lshell32

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <richedit.h>
#include <shlobj.h>
#include <initguid.h>
#include <ole2.h>
#include "webview2/build/native/include/WebView2.h"

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <map>
#include <set>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>
#include <cctype>
#include <exception>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// ─── WM_APP messages ────────────────────────────────────────────────────────
#define WM_IMAP_FOLDERS    (WM_APP + 1)   // wParam=vector<string>*
#define WM_IMAP_HEADERS    (WM_APP + 2)   // wParam=vector<MailHeader>*
#define WM_IMAP_BODY       (WM_APP + 3)   // wParam=std::string* (body text)
#define WM_STATUS_TEXT     (WM_APP + 4)   // wParam=std::wstring*
#define WM_SMTP_DONE            (WM_APP + 5)   // wParam=0 ok, 1 error; lParam=std::wstring*
#define WM_IMAP_HEADERS_APPEND  (WM_APP + 6)   // wParam=vector<MailHeader>* (append to list)
#define WM_IMAP_DELETE_DONE     (WM_APP + 7)   // wParam=row_index deleted (-1=failed)
#define WM_IMAP_READ_UPDATE     (WM_APP + 8)   // wParam=row_index now marked read
#define WM_DELETE_PROGRESS      (WM_APP + 9)   // wParam=current pct (0-100), lParam=unused

// ─── Constants ───────────────────────────────────────────────────────────────
static const char  IMAP_HOST[] = "imap.mail.me.com";
static const int   IMAP_PORT   = 993;
static const char  SMTP_HOST[] = "smtp.mail.me.com";
static const int   SMTP_PORT   = 587;

// ─── Debug logging ───────────────────────────────────────────────────────────
static void debug_log(const std::string& message) {
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    std::string log_path = std::string(temp_path) + "fastmail_log.txt";
    HANDLE file_handle = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                     nullptr, OPEN_ALWAYS, 0, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) return;
    SetFilePointer(file_handle, 0, nullptr, FILE_END);
    DWORD written;
    std::string line = message + "\r\n";
    WriteFile(file_handle, line.data(), (DWORD)line.size(), &written, nullptr);
    CloseHandle(file_handle);
}

// ─── Drag-and-drop state ─────────────────────────────────────────────────────
static bool       g_drag_active       = false;
static HTREEITEM  g_drag_drop_target  = nullptr;
static std::vector<std::string> g_drag_uids;
static HCURSOR    g_cursor_no         = nullptr;
static HCURSOR    g_cursor_move       = nullptr;
static HIMAGELIST g_drag_image_list   = nullptr;  // ghost image during drag

static LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* exception_info) {
    char message[256];
    snprintf(message, sizeof(message),
             "CRASH: SEH exception 0x%08X at 0x%p",
             exception_info->ExceptionRecord->ExceptionCode,
             exception_info->ExceptionRecord->ExceptionAddress);
    debug_log(message);
    MessageBoxA(nullptr, message, "FastMail Crash", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ─── Utility: narrow/wide conversion ─────────────────────────────────────────
static std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string result(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), result.data(), needed, nullptr, nullptr);
    return result;
}
static std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring result(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), result.data(), needed);
    return result;
}

static std::string to_lower(std::string str) {
    for (char& character : str) character = (char)tolower((unsigned char)character);
    return str;
}
static std::string trim_string(const std::string& source) {
    size_t start = source.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = source.find_last_not_of(" \t\r\n");
    return source.substr(start, end - start + 1);
}

// ─── Base64 ──────────────────────────────────────────────────────────────────
static const char BASE64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string encode_base64(const std::string& input) {
    std::string result;
    int val = 0, valb = -6;
    for (unsigned char byte_char : input) {
        val = (val << 8) + byte_char;
        valb += 8;
        while (valb >= 0) {
            result.push_back(BASE64_CHARS[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) result.push_back(BASE64_CHARS[((val << 8) >> (valb + 8)) & 0x3F]);
    while (result.size() % 4) result.push_back('=');
    return result;
}

static std::string decode_base64(const std::string& input) {
    std::vector<int> decode_table(256, -1);
    for (int index = 0; index < 64; index++) decode_table[(unsigned char)BASE64_CHARS[index]] = index;
    std::string result;
    int val = 0, valb = -8;
    for (unsigned char byte_char : input) {
        if (decode_table[byte_char] == -1) continue;
        val = (val << 6) + decode_table[byte_char];
        valb += 6;
        if (valb >= 0) {
            result.push_back((char)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return result;
}

// ─── Quoted-Printable decode ─────────────────────────────────────────────────
static std::string decode_quoted_printable(const std::string& input) {
    std::string result;
    for (size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '=' && index + 2 < input.size()) {
            if (input[index + 1] == '\r' || input[index + 1] == '\n') {
                // soft line break
                if (input[index + 1] == '\r' && index + 2 < input.size() && input[index + 2] == '\n') index += 2;
                else index += 1;
            } else {
                char hex_str[3] = { input[index + 1], input[index + 2], 0 };
                result.push_back((char)strtol(hex_str, nullptr, 16));
                index += 2;
            }
        } else {
            result.push_back(input[index]);
        }
    }
    return result;
}

// ─── Charset conversion ──────────────────────────────────────────────────────
static UINT charset_name_to_codepage(const std::string& charset_lower) {
    if (charset_lower.find("utf-8")        != std::string::npos) return CP_UTF8;
    if (charset_lower.find("utf8")         != std::string::npos) return CP_UTF8;
    if (charset_lower.find("windows-1251") != std::string::npos) return 1251;
    if (charset_lower.find("cp1251")       != std::string::npos) return 1251;
    if (charset_lower.find("windows-1252") != std::string::npos) return 1252;
    if (charset_lower.find("cp1252")       != std::string::npos) return 1252;
    if (charset_lower.find("koi8-r")       != std::string::npos) return 20866;
    if (charset_lower.find("koi8r")        != std::string::npos) return 20866;
    if (charset_lower.find("koi8-u")       != std::string::npos) return 21866;
    if (charset_lower.find("iso-8859-5")   != std::string::npos) return 28595;
    if (charset_lower.find("iso-8859-2")   != std::string::npos) return 28592;
    if (charset_lower.find("iso-8859-1")   != std::string::npos) return 28591;
    // ASCII is a subset of UTF-8: treat declared-ASCII content as UTF-8 so that
    // emails which claim ASCII but actually contain UTF-8 (smart quotes, nbsp, etc.)
    // are decoded correctly instead of being mangled by codepage 20127.
    if (charset_lower.find("us-ascii")     != std::string::npos) return CP_UTF8;
    if (charset_lower.find("ascii")        != std::string::npos) return CP_UTF8;
    return 1252; // safe fallback for unknown Western charsets
}

static std::string convert_charset_to_utf8(const std::string& raw_bytes, const std::string& charset_lower) {
    if (raw_bytes.empty()) return {};
    UINT codepage = charset_name_to_codepage(charset_lower);
    if (codepage == CP_UTF8) return raw_bytes;
    int wide_len = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS,
                                       raw_bytes.data(), (int)raw_bytes.size(), nullptr, 0);
    if (wide_len <= 0) {
        // Fallback: try without strict validation
        wide_len = MultiByteToWideChar(codepage, 0,
                                       raw_bytes.data(), (int)raw_bytes.size(), nullptr, 0);
        if (wide_len <= 0) return raw_bytes;
    }
    std::wstring wide(wide_len, L'\0');
    MultiByteToWideChar(codepage, 0, raw_bytes.data(), (int)raw_bytes.size(), wide.data(), wide_len);
    return wide_to_utf8(wide);
}

// ─── RFC2047 encoded-word decode ─────────────────────────────────────────────
static std::string decode_rfc2047(const std::string& input) {
    std::string result;
    size_t pos = 0;
    while (pos < input.size()) {
        size_t start = input.find("=?", pos);
        if (start == std::string::npos) {
            result += input.substr(pos);
            break;
        }
        result += input.substr(pos, start - pos);
        size_t charset_end = input.find('?', start + 2);
        if (charset_end == std::string::npos) { result += input.substr(start); break; }
        size_t encoding_end = input.find('?', charset_end + 1);
        if (encoding_end == std::string::npos) { result += input.substr(start); break; }
        size_t word_end = input.find("?=", encoding_end + 1);
        if (word_end == std::string::npos) { result += input.substr(start); break; }
        char encoding_char = (char)tolower((unsigned char)input[charset_end + 1]);
        std::string encoded_text = input.substr(encoding_end + 1, word_end - encoding_end - 1);
        std::string decoded_text;
        if (encoding_char == 'b') {
            decoded_text = decode_base64(encoded_text);
        } else if (encoding_char == 'q') {
            for (char& ch : encoded_text) if (ch == '_') ch = ' ';
            decoded_text = decode_quoted_printable(encoded_text);
        } else {
            decoded_text = encoded_text;
        }
        std::string charset = to_lower(input.substr(start + 2, charset_end - start - 2));
        result += convert_charset_to_utf8(decoded_text, charset);
        pos = word_end + 2;
    }
    return result;
}

// Returns true if the buffer looks like binary (non-text) data.
static bool looks_like_binary(const std::string& text) {
    if (text.size() < 16) return false;
    size_t non_text_count = 0;
    for (unsigned char byte_value : text) {
        // Allow: \t \n \r and printable ASCII and high bytes (UTF-8 / extended)
        bool is_text_byte = (byte_value == '\t' || byte_value == '\n' || byte_value == '\r'
                             || byte_value >= 32);
        if (!is_text_byte) ++non_text_count;
    }
    // More than 5% non-text bytes → treat as binary
    return non_text_count > text.size() / 20;
}

// ─── HTML entity decode (used by strip_html) ─────────────────────────────────
static void append_html_entity(std::string& result, const std::string& entity) {
    std::string lower = to_lower(entity);
    // Named entities
    if (lower == "amp")        { result += '&';             return; }
    if (lower == "lt")         { result += '<';             return; }
    if (lower == "gt")         { result += '>';             return; }
    if (lower == "quot")       { result += '"';             return; }
    if (lower == "apos")       { result += '\'';            return; }
    if (lower == "nbsp")       { result += ' ';             return; }
    if (lower == "copy")       { result += "\xC2\xA9";      return; } // ©
    if (lower == "reg")        { result += "\xC2\xAE";      return; } // ®
    if (lower == "mdash")      { result += "\xE2\x80\x94";  return; } // —
    if (lower == "ndash")      { result += "\xE2\x80\x93";  return; } // –
    if (lower == "lsquo")      { result += "\xE2\x80\x98";  return; } // '
    if (lower == "rsquo")      { result += "\xE2\x80\x99";  return; } // '
    if (lower == "ldquo")      { result += "\xE2\x80\x9C";  return; } // "
    if (lower == "rdquo")      { result += "\xE2\x80\x9D";  return; } // "
    if (lower == "bull" || lower == "bullet") { result += "\xE2\x80\xA2"; return; } // •
    if (lower == "hellip")     { result += "\xE2\x80\xA6";  return; } // …
    if (lower == "trade")      { result += "\xE2\x84\xA2";  return; } // ™
    if (lower == "euro")       { result += "\xE2\x82\xAC";  return; } // €
    // Numeric entities: &#NNN; or &#xNN;
    if (!entity.empty() && entity[0] == '#') {
        unsigned long codepoint = 0;
        try {
            if (entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X'))
                codepoint = std::stoul(entity.substr(2), nullptr, 16);
            else if (entity.size() > 1)
                codepoint = std::stoul(entity.substr(1), nullptr, 10);
        } catch (...) { codepoint = 0; }
        // Encode codepoint as UTF-8
        if (codepoint >= 32 && codepoint < 0x80) {
            result += (char)codepoint;
        } else if (codepoint >= 0x80 && codepoint < 0x800) {
            result += (char)(0xC0 | (codepoint >> 6));
            result += (char)(0x80 | (codepoint & 0x3F));
        } else if (codepoint >= 0x800 && codepoint < 0x10000) {
            result += (char)(0xE0 | (codepoint >> 12));
            result += (char)(0x80 | ((codepoint >> 6) & 0x3F));
            result += (char)(0x80 | (codepoint & 0x3F));
        }
        return;
    }
    // Unknown entity — emit as-is
    result += '&';
    result += entity;
    result += ';';
}

// ─── HTML strip ──────────────────────────────────────────────────────────────
// Returns the tag name (lowercase, without < / >) from a collected tag buffer.
static std::string html_tag_name(const std::string& lower_tag) {
    size_t start = 1;
    if (start < lower_tag.size() && lower_tag[start] == '/') ++start;
    size_t end = start;
    while (end < lower_tag.size() && lower_tag[end] != '>' && lower_tag[end] != ' '
           && lower_tag[end] != '\t' && lower_tag[end] != '\r' && lower_tag[end] != '\n')
        ++end;
    return lower_tag.substr(start, end - start);
}

static std::string strip_html(const std::string& html) {
    std::string result;
    bool in_tag       = false;
    bool in_skip      = false;  // inside <head>, <style>, or <script> block content
    std::string tag_buf;

    for (size_t index = 0; index < html.size(); ++index) {
        char ch = html[index];
        if (in_tag) {
            tag_buf += ch;
            if (ch == '>') {
                in_tag = false;
                std::string lower_tag = to_lower(tag_buf);
                std::string tag_name  = html_tag_name(lower_tag);
                bool is_close = lower_tag.size() > 1 && lower_tag[1] == '/';

                // <head> <style> <script> — skip their content entirely
                if (tag_name == "head" || tag_name == "style" || tag_name == "script") {
                    in_skip = !is_close;
                }

                if (!in_skip) {
                    // Block-level tags → newline
                    if (tag_name == "br" || tag_name == "p" || tag_name == "div" ||
                        tag_name == "tr" || tag_name == "li" || tag_name == "hr" ||
                        tag_name == "h1" || tag_name == "h2" || tag_name == "h3" ||
                        tag_name == "h4" || tag_name == "h5" || tag_name == "h6") {
                        result += '\n';
                    }
                }
                tag_buf.clear();
            }
        } else if (ch == '<') {
            in_tag  = true;
            tag_buf = '<';
        } else if (!in_skip) {
            if (ch == '&') {
                size_t semi = html.find(';', index + 1);
                if (semi != std::string::npos && semi - index <= 12) {
                    append_html_entity(result, html.substr(index + 1, semi - index - 1));
                    index = semi;
                } else {
                    result += ch;
                }
            } else {
                result += ch;
            }
        }
    }
    return result;
}

// ─── Clean up decoded text body before display ───────────────────────────────
static std::string clean_text_body(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    int blank_lines = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t line_end = text.find('\n', pos);
        bool last_line  = (line_end == std::string::npos);
        std::string line = last_line ? text.substr(pos) : text.substr(pos, line_end - pos);
        // Strip trailing \r and spaces
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();

        // Skip bare URL-only lines longer than 120 chars (SafeLinks, tracking URLs)
        // that appear in text/plain as raw noise.  Lines with surrounding text keep.
        bool is_url_line = (line.size() > 120 && line.find(' ') == std::string::npos
                            && (line.front() == '<' || line.substr(0, 4) == "http"));

        bool is_blank = line.empty();
        if (is_blank) {
            ++blank_lines;
            if (blank_lines <= 2) result += '\n';
        } else if (!is_url_line) {
            blank_lines = 0;
            result += line;
            result += '\n';
        }
        if (last_line) break;
        pos = line_end + 1;
    }
    // Trim leading blank lines
    size_t first_text = result.find_first_not_of("\r\n");
    if (first_text != std::string::npos) result = result.substr(first_text);
    return result;
}

// ─── TLS Connection (Schannel) ───────────────────────────────────────────────
class TlsConnection {
public:
    TlsConnection() : socket_(INVALID_SOCKET), context_valid_(false), cred_valid_(false) {}

    ~TlsConnection() {
        close();
    }

    bool connect_direct(const char* hostname, int port) {
        try {
            hostname_ = hostname;
            if (!connect_tcp(hostname, port)) return false;
            return do_tls_handshake();
        } catch (...) {
            return false;
        }
    }

    bool connect_starttls(const char* hostname, int port,
                          std::function<bool(TlsConnection*)> plain_exchange) {
        try {
            hostname_ = hostname;
            if (!connect_tcp(hostname, port)) return false;
            // Do plain exchange first
            if (!plain_exchange(this)) return false;
            return do_tls_handshake();
        } catch (...) {
            return false;
        }
    }

    bool send_data(const std::string& data) {
        if (!context_valid_) return false;
        size_t offset = 0;
        while (offset < data.size()) {
            size_t chunk = std::min(data.size() - offset, (size_t)stream_sizes_.cbMaximumMessage);
            SecBuffer send_buffers[4] = {};
            BYTE* header_buffer  = nullptr;
            BYTE* data_buffer    = nullptr;
            BYTE* trailer_buffer = nullptr;
            try {
                header_buffer  = new BYTE[stream_sizes_.cbHeader];
                data_buffer    = new BYTE[chunk];
                trailer_buffer = new BYTE[stream_sizes_.cbTrailer];
            } catch (...) {
                delete[] header_buffer;
                delete[] data_buffer;
                delete[] trailer_buffer;
                return false;
            }
            send_buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
            send_buffers[0].cbBuffer   = stream_sizes_.cbHeader;
            send_buffers[0].pvBuffer   = header_buffer;
            send_buffers[1].BufferType = SECBUFFER_DATA;
            send_buffers[1].cbBuffer   = (ULONG)chunk;
            send_buffers[1].pvBuffer   = data_buffer;
            memcpy(send_buffers[1].pvBuffer, data.data() + offset, chunk);
            send_buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
            send_buffers[2].cbBuffer   = stream_sizes_.cbTrailer;
            send_buffers[2].pvBuffer   = trailer_buffer;
            send_buffers[3].BufferType = SECBUFFER_EMPTY;

            SecBufferDesc send_desc = { SECBUFFER_VERSION, 4, send_buffers };
            SECURITY_STATUS status = EncryptMessage(&context_, 0, &send_desc, 0);
            if (SUCCEEDED(status)) {
                size_t total = send_buffers[0].cbBuffer + send_buffers[1].cbBuffer + send_buffers[2].cbBuffer;
                bool tcp_ok = false;
                try {
                    std::string encrypted_data(total, '\0');
                    memcpy(encrypted_data.data(), send_buffers[0].pvBuffer, send_buffers[0].cbBuffer);
                    memcpy(encrypted_data.data() + send_buffers[0].cbBuffer, send_buffers[1].pvBuffer, send_buffers[1].cbBuffer);
                    memcpy(encrypted_data.data() + send_buffers[0].cbBuffer + send_buffers[1].cbBuffer,
                           send_buffers[2].pvBuffer, send_buffers[2].cbBuffer);
                    tcp_ok = tcp_send(encrypted_data);
                } catch (...) {
                    tcp_ok = false;
                }
                delete[] (BYTE*)send_buffers[0].pvBuffer;
                delete[] (BYTE*)send_buffers[1].pvBuffer;
                delete[] (BYTE*)send_buffers[2].pvBuffer;
                if (!tcp_ok) return false;
            } else {
                delete[] (BYTE*)send_buffers[0].pvBuffer;
                delete[] (BYTE*)send_buffers[1].pvBuffer;
                delete[] (BYTE*)send_buffers[2].pvBuffer;
                return false;
            }
            offset += chunk;
        }
        return true;
    }

    bool send_plain(const std::string& data) {
        return tcp_send(data);
    }

    // Read a line ending with \n (may include \r\n)
    std::string recv_line() {
        for (;;) {
            auto newline_pos = plain_buf_.find('\n');
            if (newline_pos != std::string::npos) {
                std::string line = plain_buf_.substr(0, newline_pos + 1);
                plain_buf_.erase(0, newline_pos + 1);
                return line;
            }
            if (!fill_plain_buf()) return {};
        }
    }

    // Read plain (for pre-TLS SMTP)
    std::string recv_plain_line() {
        for (;;) {
            auto newline_pos = plain_buf_.find('\n');
            if (newline_pos != std::string::npos) {
                std::string line = plain_buf_.substr(0, newline_pos + 1);
                plain_buf_.erase(0, newline_pos + 1);
                return line;
            }
            char buf[4096];
            int received = ::recv(socket_, buf, sizeof(buf), 0);
            if (received <= 0) return {};
            plain_buf_.append(buf, received);
        }
    }

    // Read exactly n bytes
    std::string recv_exact(size_t count) {
        while (plain_buf_.size() < count) {
            if (!fill_plain_buf()) return {};
        }
        std::string result = plain_buf_.substr(0, count);
        plain_buf_.erase(0, count);
        return result;
    }

    void close() {
        if (context_valid_) { DeleteSecurityContext(&context_); context_valid_ = false; }
        if (cred_valid_)    { FreeCredentialsHandle(&credentials_); cred_valid_ = false; }
        if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; }
        raw_buf_.clear();
        plain_buf_.clear();
    }

    bool is_connected() const { return socket_ != INVALID_SOCKET; }

private:
    SOCKET         socket_;
    CtxtHandle     context_;
    CredHandle     credentials_;
    SecPkgContext_StreamSizes stream_sizes_;
    bool           context_valid_;
    bool           cred_valid_;
    std::string    hostname_;
    std::string    raw_buf_;   // encrypted bytes from socket not yet decrypted
    std::string    plain_buf_; // decrypted bytes ready to read

    bool connect_tcp(const char* hostname, int port) {
        debug_log(std::string("TCP: connecting to ") + hostname + ":" + std::to_string(port));
        addrinfo hints = {};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family   = AF_UNSPEC;
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%d", port);
        addrinfo* address_info = nullptr;
        if (getaddrinfo(hostname, port_str, &hints, &address_info) != 0) {
            debug_log("TCP: getaddrinfo failed"); return false;
        }
        debug_log("TCP: DNS resolved, creating socket");
        socket_ = ::socket(address_info->ai_family, address_info->ai_socktype, address_info->ai_protocol);
        if (socket_ == INVALID_SOCKET) { freeaddrinfo(address_info); debug_log("TCP: socket() failed"); return false; }
        debug_log("TCP: calling connect()");
        bool connected = (::connect(socket_, address_info->ai_addr, (int)address_info->ai_addrlen) == 0);
        freeaddrinfo(address_info);
        if (!connected) { debug_log("TCP: connect() failed"); return false; }
        // Set receive timeout so recv() never blocks forever
        DWORD recv_timeout_ms = 15000;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, (char*)&recv_timeout_ms, sizeof(recv_timeout_ms));
        debug_log("TCP: connected, recv timeout=15s");
        return true;
    }

    bool do_tls_handshake() {
        debug_log("TLS: starting handshake for " + hostname_);
        SCHANNEL_CRED schannel_cred = {};
        schannel_cred.dwVersion = SCHANNEL_CRED_VERSION;
        schannel_cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT;
        schannel_cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION;

        SECURITY_STATUS status = AcquireCredentialsHandleA(
            nullptr, (LPSTR)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, nullptr,
            &schannel_cred, nullptr, nullptr, &credentials_, nullptr);
        if (status != SEC_E_OK) {
            char err[64]; snprintf(err, sizeof(err), "TLS: AcquireCredentialsHandle failed 0x%08X", (unsigned)status);
            debug_log(err); return false;
        }
        cred_valid_ = true;
        debug_log("TLS: credentials acquired");

        // No ISC_REQ_ALLOCATE_MEMORY — we manage all memory so SECBUFFER_EXTRA
        // points into our own buffers instead of Schannel-internal memory.
        DWORD isc_req_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                              ISC_REQ_CONFIDENTIALITY | ISC_REQ_STREAM;

        std::vector<BYTE> initial_out_token(32768, 0);
        SecBuffer out_buffer = { 32768, SECBUFFER_TOKEN, initial_out_token.data() };
        SecBufferDesc out_desc = { SECBUFFER_VERSION, 1, &out_buffer };

        DWORD initial_context_flags = 0;
        status = InitializeSecurityContextA(
            &credentials_, nullptr, (LPSTR)hostname_.c_str(),
            isc_req_flags, 0, SECURITY_NATIVE_DREP, nullptr, 0,
            &context_, &out_desc, &initial_context_flags, nullptr);
        context_valid_ = (status == SEC_I_CONTINUE_NEEDED || SUCCEEDED(status));

        if (out_buffer.cbBuffer > 0) {
            tcp_send(std::string((char*)out_buffer.pvBuffer, out_buffer.cbBuffer));
        }

        if (status != SEC_I_CONTINUE_NEEDED) {
            char err[80]; snprintf(err, sizeof(err), "TLS: initial ISC failed 0x%08X flags=0x%08X", (unsigned)status, initial_context_flags);
            debug_log(err); return false;
        }
        debug_log("TLS: client hello sent, entering handshake loop");

        // Handshake loop — only recv() when raw_buf_ is empty to avoid blocking on
        // application data (e.g. IMAP greeting) that arrived with the last TLS record.
        for (;;) {
            if (raw_buf_.empty()) {
                char socket_buf[16384];
                int received = ::recv(socket_, socket_buf, sizeof(socket_buf), 0);
                if (received <= 0) return false;
                raw_buf_.append(socket_buf, received);
            }

            // Give ISC a copy of raw_buf_ so raw_buf_ stays untouched regardless of
            // outcome; SECBUFFER_EXTRA then points into isc_input (our memory), not
            // into Schannel-internal allocations.
            std::string isc_input = raw_buf_;

            SecBuffer in_buffers[2] = {};
            in_buffers[0] = { (ULONG)isc_input.size(), SECBUFFER_TOKEN, isc_input.data() };
            in_buffers[1] = { 0, SECBUFFER_EMPTY, nullptr };
            SecBufferDesc in_desc  = { SECBUFFER_VERSION, 2, in_buffers };

            // Pre-allocate output buffer (required without ISC_REQ_ALLOCATE_MEMORY)
            std::vector<BYTE> loop_out_token(32768, 0);
            SecBuffer out_buffers[2] = {};
            out_buffers[0] = { 32768, SECBUFFER_TOKEN, loop_out_token.data() };
            out_buffers[1] = { 0,     SECBUFFER_EMPTY, nullptr };
            SecBufferDesc out_desc2 = { SECBUFFER_VERSION, 2, out_buffers };

            DWORD out_flags = 0;
            status = InitializeSecurityContextA(
                &credentials_, &context_, nullptr,
                isc_req_flags, 0, SECURITY_NATIVE_DREP, &in_desc, 0,
                nullptr, &out_desc2, &out_flags, nullptr);

            // Send handshake data only when ISC explicitly produced it.
            // For SEC_E_INCOMPLETE_MESSAGE, cbBuffer stays at the pre-allocated size
            // (32768) because ISC doesn't reset it — sending that would corrupt the
            // TLS stream and cause the server to send a handshake_failure alert.
            if ((status == SEC_I_CONTINUE_NEEDED || status == SEC_E_OK)
                && out_buffers[0].cbBuffer > 0) {
                tcp_send(std::string((char*)out_buffers[0].pvBuffer, out_buffers[0].cbBuffer));
            }

            {
                char dbg[200];
                snprintf(dbg, sizeof(dbg),
                    "ISC: status=0x%08X isc_input=%zu in[0]={t=%d cb=%u} in[1]={t=%d cb=%u} out[0].cb=%u",
                    (unsigned)status, isc_input.size(),
                    in_buffers[0].BufferType, in_buffers[0].cbBuffer,
                    in_buffers[1].BufferType, in_buffers[1].cbBuffer,
                    out_buffers[0].cbBuffer);
                debug_log(dbg);
            }

            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                // Partial TLS record — raw_buf_ is untouched, just read more
                debug_log("TLS: incomplete message, reading more");
                char socket_buf[16384];
                int received = ::recv(socket_, socket_buf, sizeof(socket_buf), 0);
                if (received <= 0) return false;
                raw_buf_.append(socket_buf, received);
                continue;
            }

            if (status == (SECURITY_STATUS)SEC_E_INVALID_TOKEN) {
                // TLS 1.3: IMAP greeting arrived as app data with server Finished.
                // raw_buf_ was never touched — decrypt_raw() can use it directly.
                debug_log("TLS: SEC_E_INVALID_TOKEN -> raw_buf_ intact, handshake done");
                break;
            }

            // SECBUFFER_EXTRA: pvBuffer may point outside isc_input (Schannel quirk).
            // Use cbBuffer to slice the tail of isc_input instead of dereferencing pvBuffer.
            raw_buf_ = "";
            for (int bi = 0; bi < 2; ++bi) {
                if (in_buffers[bi].BufferType == SECBUFFER_EXTRA
                    && in_buffers[bi].cbBuffer > 0
                    && in_buffers[bi].cbBuffer <= isc_input.size()) {
                    size_t extra_offset = isc_input.size() - in_buffers[bi].cbBuffer;
                    raw_buf_ = isc_input.substr(extra_offset);
                    break;
                }
            }

            if (status == SEC_E_OK) { debug_log("TLS: handshake complete"); break; }
            if (status != SEC_I_CONTINUE_NEEDED) {
                char err[80]; snprintf(err, sizeof(err), "TLS: handshake loop ISC failed 0x%08X", (unsigned)status);
                debug_log(err); return false;
            }
            debug_log("TLS: handshake round trip");
        }

        QueryContextAttributes(&context_, SECPKG_ATTR_STREAM_SIZES, &stream_sizes_);
        {
            char info[256];
            snprintf(info, sizeof(info), "TLS: post-handshake raw_buf_=%zu plain_buf_=%zu",
                     raw_buf_.size(), plain_buf_.size());
            debug_log(info);
            // Hex dump first 32 bytes of raw_buf_
            if (!raw_buf_.empty()) {
                std::string hex;
                for (size_t hi = 0; hi < std::min(raw_buf_.size(), (size_t)32); ++hi) {
                    char byte_str[4];
                    snprintf(byte_str, sizeof(byte_str), "%02X ", (unsigned char)raw_buf_[hi]);
                    hex += byte_str;
                }
                debug_log("raw_buf_ hex: " + hex);
            }
        }
        // Decrypt any extra bytes that arrived during handshake
        if (!raw_buf_.empty()) {
            bool ok = decrypt_raw();
            char info[128];
            snprintf(info, sizeof(info), "TLS: post-handshake decrypt ok=%d plain_buf_=%zu",
                     (int)ok, plain_buf_.size());
            debug_log(info);
        }
        return true;
    }

    bool tcp_send(const std::string& data) {
        size_t total_sent = 0;
        while (total_sent < data.size()) {
            int sent = ::send(socket_, data.data() + total_sent, (int)(data.size() - total_sent), 0);
            if (sent <= 0) return false;
            total_sent += sent;
        }
        return true;
    }

    bool fill_plain_buf() {
        // Loop: decrypt whatever is in raw_buf_, then read more if needed.
        // SEC_E_INCOMPLETE_MESSAGE means the current raw_buf_ doesn't contain
        // a full TLS record — keep calling recv() until we have one.
        for (;;) {
            if (!raw_buf_.empty() && decrypt_raw()) return true;
            char socket_buf[16384];
            int received = ::recv(socket_, socket_buf, sizeof(socket_buf), 0);
            if (received <= 0) return false;
            raw_buf_.append(socket_buf, received);
        }
    }

    bool decrypt_raw() {
        bool decrypted_any = false;
        for (;;) {
            if (raw_buf_.empty()) break;
            SecBuffer decr_buffers[4] = {};
            decr_buffers[0].BufferType = SECBUFFER_DATA;
            decr_buffers[0].cbBuffer   = (ULONG)raw_buf_.size();
            decr_buffers[0].pvBuffer   = raw_buf_.data();
            decr_buffers[1].BufferType = SECBUFFER_EMPTY;
            decr_buffers[2].BufferType = SECBUFFER_EMPTY;
            decr_buffers[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc decr_desc = { SECBUFFER_VERSION, 4, decr_buffers };

            SECURITY_STATUS status = DecryptMessage(&context_, &decr_desc, 0, nullptr);
            {
                char info[80];
                snprintf(info, sizeof(info), "decrypt_raw: status=0x%08X raw=%zu", (unsigned)status, raw_buf_.size());
                debug_log(info);
            }
            if (status == SEC_E_INCOMPLETE_MESSAGE) break;
            if (status == SEC_I_CONTEXT_EXPIRED) break;
            if (FAILED(status)) break;

            // Extract decrypted data
            for (int bi = 0; bi < 4; ++bi) {
                if (decr_buffers[bi].BufferType == SECBUFFER_DATA && decr_buffers[bi].cbBuffer > 0) {
                    plain_buf_.append((char*)decr_buffers[bi].pvBuffer, decr_buffers[bi].cbBuffer);
                    decrypted_any = true;
                }
            }
            // Handle extra (undecrypted leftover)
            raw_buf_.clear();
            for (int bi = 0; bi < 4; ++bi) {
                if (decr_buffers[bi].BufferType == SECBUFFER_EXTRA && decr_buffers[bi].cbBuffer > 0) {
                    raw_buf_.assign((char*)decr_buffers[bi].pvBuffer, decr_buffers[bi].cbBuffer);
                }
            }
        }
        return decrypted_any;
    }
};

// ─── WebView2 Browser Host ────────────────────────────────────────────────────
class BrowserHost : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                    public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    BrowserHost() = default;
    ~BrowserHost() { destroy(); }

    void create(HWND parent, int x, int y, int cx, int cy) {
        parent_hwnd_ = parent;
        x_ = x; y_ = y; cx_ = cx; cy_ = cy;

        // Create a host child window for WebView2
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FastMailWebView2Host";
        RegisterClassExW(&wc);  // ignore failure — class may already exist

        host_hwnd_ = CreateWindowExW(0, L"FastMailWebView2Host", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            x, y, cx, cy, parent, nullptr, GetModuleHandleW(nullptr), nullptr);

        // User data folder in the exe directory
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        std::wstring user_data = exe_path;
        size_t sep = user_data.rfind(L'\\');
        if (sep != std::wstring::npos) user_data.resize(sep);
        user_data += L"\\WebView2Data";

        // Dynamic load of WebView2Loader.dll to avoid needing an import lib
        typedef HRESULT (*PFN_CreateEnv)(PCWSTR, PCWSTR,
            ICoreWebView2EnvironmentOptions*,
            ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

        HMODULE wv2_module = LoadLibraryW(L"WebView2Loader.dll");
        auto create_env_fn = wv2_module
            ? (PFN_CreateEnv)GetProcAddress(wv2_module, "CreateCoreWebView2EnvironmentWithOptions")
            : nullptr;
        if (create_env_fn) {
            HRESULT hr = create_env_fn(nullptr, user_data.c_str(), nullptr, this);
            if (FAILED(hr)) {
                debug_log("WebView2: CreateCoreWebView2EnvironmentWithOptions failed hr=" +
                          std::to_string(hr));
            }
        } else {
            debug_log("WebView2: failed to load WebView2Loader.dll or find entry point");
        }
    }

    void navigate_html(const std::string& html_utf8) {
        // Patch charset declarations to UTF-8 before converting to wide string.
        // WebView2 NavigateToString delivers content as UTF-16 but still honours
        // the HTML charset meta tag — if it says koi8-r or windows-1251 the text
        // will be re-decoded incorrectly.  Force it to UTF-8 everywhere.
        std::string patched = html_utf8;
        std::string lower   = to_lower(patched);
        {
            std::string result;
            result.reserve(patched.size() + 32);
            size_t search = 0;
            while (search < patched.size()) {
                size_t cp = lower.find("charset=", search);
                if (cp == std::string::npos) { result += patched.substr(search); break; }
                result += patched.substr(search, cp - search);
                result += "charset=";
                size_t vs = cp + 8;
                if (vs < patched.size() && (patched[vs] == '"' || patched[vs] == '\'')) {
                    char q = patched[vs]; result += q;
                    size_t ve = patched.find(q, vs + 1);
                    if (ve == std::string::npos) ve = patched.size();
                    result += "UTF-8";
                    search = ve;
                } else {
                    result += "UTF-8";
                    size_t ve = patched.find_first_of(" ;\"'>", vs);
                    if (ve == std::string::npos) ve = patched.size();
                    search = ve;
                }
            }
            patched = std::move(result);
            lower   = to_lower(patched);
        }
        // Inject charset meta if none found
        if (lower.find("charset=") == std::string::npos) {
            size_t head = lower.find("<head");
            if (head != std::string::npos) {
                size_t end = patched.find('>', head);
                if (end != std::string::npos)
                    patched.insert(end + 1, "<meta charset=\"UTF-8\">");
            } else {
                patched = "<meta charset=\"UTF-8\">" + patched;
            }
        }

        pending_html_ = utf8_to_wide(patched);
        if (webview_) {
            webview_->NavigateToString(pending_html_.c_str());
            pending_html_.clear();
        }
    }

    void navigate_text(const std::string& text_utf8) {
        // Build HTML, preserving existing HTML entities to avoid double-encoding.
        // (Some plain-text emails contain &lt; &amp; etc. from quoted HTML.)
        std::wstring html =
            L"<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
            L"<style>body{font-family:'Segoe UI',Arial,sans-serif;font-size:13px;"
            L"margin:8px;white-space:pre-wrap;word-wrap:break-word;color:#1a1a1a;}"
            L"a{color:#0066cc;}</style></head><body>";

        static const char* known_entities[] = {
            "&lt;", "&gt;", "&amp;", "&quot;", "&apos;",
            "&nbsp;", "&mdash;", "&ndash;", "&hellip;", nullptr
        };

        size_t pos = 0;
        while (pos < text_utf8.size()) {
            char ch = text_utf8[pos];
            if (ch == '&') {
                // Pass through known HTML entities without re-encoding
                bool is_entity = false;
                for (int entity_index = 0; known_entities[entity_index]; ++entity_index) {
                    size_t entity_len = strlen(known_entities[entity_index]);
                    if (text_utf8.compare(pos, entity_len, known_entities[entity_index]) == 0) {
                        for (size_t char_index = 0; char_index < entity_len; ++char_index)
                            html += (wchar_t)(unsigned char)text_utf8[pos + char_index];
                        pos += entity_len;
                        is_entity = true;
                        break;
                    }
                }
                if (!is_entity) { html += L"&amp;"; ++pos; }
            } else if (ch == '<') {
                html += L"&lt;"; ++pos;
            } else if (ch == '>') {
                html += L"&gt;"; ++pos;
            } else {
                html += (wchar_t)(unsigned char)ch; ++pos;
            }
        }
        html += L"</body></html>";
        pending_html_ = html;
        if (webview_) {
            webview_->NavigateToString(pending_html_.c_str());
            pending_html_.clear();
        }
    }

    void resize(int x, int y, int cx, int cy) {
        x_ = x; y_ = y; cx_ = cx; cy_ = cy;
        if (host_hwnd_) MoveWindow(host_hwnd_, x, y, cx, cy, TRUE);
        if (controller_) {
            RECT bounds = {0, 0, cx, cy};
            controller_->put_Bounds(bounds);
        }
    }

    HWND host_window() const { return host_hwnd_; }

    void destroy() {
        if (controller_) { controller_->Close(); controller_->Release(); controller_ = nullptr; }
        if (webview_)    { webview_->Release(); webview_ = nullptr; }
        if (host_hwnd_)  { DestroyWindow(host_hwnd_); host_hwnd_ = nullptr; }
    }

    // ── IUnknown ──────────────────────────────────────────────────────────────
    ULONG STDMETHODCALLTYPE AddRef()  override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) ||
            IsEqualIID(riid, IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    // ── Environment completed handler ─────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                     ICoreWebView2Environment* env) override {
        if (FAILED(result) || !env || !host_hwnd_) {
            debug_log("WebView2: environment creation failed hr=" + std::to_string(result));
            return S_OK;
        }
        env->CreateCoreWebView2Controller(host_hwnd_, this);
        return S_OK;
    }

    // ── Controller completed handler ──────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                     ICoreWebView2Controller* controller) override {
        if (FAILED(result) || !controller) {
            debug_log("WebView2: controller creation failed hr=" + std::to_string(result));
            return S_OK;
        }
        controller_ = controller;
        controller_->AddRef();
        controller_->get_CoreWebView2(&webview_);

        // Set bounds
        RECT bounds = {0, 0, cx_, cy_};
        controller_->put_Bounds(bounds);

        // Disable context menu and status bar for cleaner look
        if (webview_) {
            ICoreWebView2Settings* settings = nullptr;
            if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
                settings->put_AreDefaultContextMenusEnabled(FALSE);
                settings->put_IsStatusBarEnabled(FALSE);
                settings->put_AreDevToolsEnabled(TRUE);
                settings->Release();
            }
        }

        // Display pending content if any
        if (!pending_html_.empty() && webview_) {
            webview_->NavigateToString(pending_html_.c_str());
            pending_html_.clear();
        } else if (webview_) {
            // Initial blank page
            webview_->NavigateToString(L"<html><body></body></html>");
        }

        debug_log("WebView2: ready");
        return S_OK;
    }

private:
    HWND     host_hwnd_    = nullptr;
    HWND     parent_hwnd_  = nullptr;
    int      x_ = 0, y_ = 0, cx_ = 400, cy_ = 300;
    LONG     ref_          = 1;
    ICoreWebView2Controller* controller_ = nullptr;
    ICoreWebView2*           webview_    = nullptr;
    std::wstring             pending_html_;
};

static BrowserHost* g_browser_host = nullptr;

// ─── External image downloader ───────────────────────────────────────────────
// Download one HTTP/HTTPS URL via WinHTTP. Returns raw bytes or empty on error.
// Skips if response > max_bytes to keep things fast.
static std::string winhttp_fetch(const std::wstring& url, size_t max_bytes = 512 * 1024) {
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t scheme[16] = {}, host[512] = {}, path[4096] = {};
    uc.lpszScheme    = scheme; uc.dwSchemeLength    = _countof(scheme);
    uc.lpszHostName  = host;   uc.dwHostNameLength  = _countof(host);
    uc.lpszUrlPath   = path;   uc.dwUrlPathLength   = _countof(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return {};

    HINTERNET session = WinHttpOpen(L"FastMail/1.0 (image fetch)",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return {};

    WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);  // 5-8 second timeouts

    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) { WinHttpCloseHandle(session); return {}; }

    DWORD req_flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return {}; }

    bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != 0
           && WinHttpReceiveResponse(req, nullptr) != 0;

    std::string body;
    if (ok) {
        DWORD status = 0, status_size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            nullptr, &status, &status_size, nullptr);
        if (status == 200 || status == 304) {
            DWORD avail = 0, got = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                if (body.size() + avail > max_bytes) break;
                size_t old_sz = body.size();
                body.resize(old_sz + avail);
                if (!WinHttpReadData(req, body.data() + old_sz, avail, &got)) break;
                body.resize(old_sz + got);
            }
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return body;
}

// Scan HTML for external <img src="http(s)://..."> and replace with data URIs.
// Runs on the IMAP worker thread; result is cached so second load is instant.
static std::string embed_external_images(const std::string& html) {
    if (html.empty()) return html;

    std::string result   = html;
    std::string lower    = to_lower(html);
    std::map<std::string, std::string> cache;  // url → data-uri (or "" if failed)

    size_t search_from = 0;
    while (search_from < result.size()) {
        // Find src=" followed by http
        size_t attr = lower.find("src=\"http", search_from);
        if (attr == std::string::npos) break;

        size_t url_start = attr + 5;  // skip src="
        size_t url_end   = result.find('"', url_start);
        if (url_end == std::string::npos) break;

        std::string url = result.substr(url_start, url_end - url_start);

        if (cache.find(url) == cache.end()) {
            // Convert ASCII URL to wide
            std::wstring wurl(url.begin(), url.end());
            std::string data = winhttp_fetch(wurl);
            if (!data.empty()) {
                // Detect MIME type from URL extension or just use jpeg as fallback
                std::string url_l = to_lower(url);
                std::string mime  = "image/jpeg";
                if (url_l.find(".png")  != std::string::npos) mime = "image/png";
                else if (url_l.find(".gif")  != std::string::npos) mime = "image/gif";
                else if (url_l.find(".webp") != std::string::npos) mime = "image/webp";
                else if (url_l.find(".svg")  != std::string::npos) mime = "image/svg+xml";
                cache[url] = "data:" + mime + ";base64," + encode_base64(data);
            } else {
                cache[url] = "";  // failed — leave original URL
            }
        }

        const std::string& data_uri = cache[url];
        if (!data_uri.empty()) {
            result.replace(url_start, url_end - url_start, data_uri);
            lower.replace(url_start, url_end - url_start, std::string(data_uri.size(), ' '));
            search_from = url_start + data_uri.size();
        } else {
            search_from = url_end + 1;
        }
    }
    return result;
}

// ─── Disk cache ───────────────────────────────────────────────────────────────
static std::wstring get_cache_dir() {
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    // Strip filename, keep directory
    std::wstring dir = exe_path;
    size_t last_sep = dir.rfind(L'\\');
    if (last_sep != std::wstring::npos) dir = dir.substr(0, last_sep);
    dir += L"\\cache";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::string xor_crypt(const std::string& data, const std::string& key) {
    if (key.empty()) return data;
    std::string result(data.size(), '\0');
    for (size_t index = 0; index < data.size(); ++index)
        result[index] = data[index] ^ key[index % key.size()];
    return result;
}

// Hash folder+uid to a safe filename (hex string, no special chars)
static std::wstring cache_filename(const std::string& folder, const std::string& uid) {
    std::string combined = folder + "\x01" + uid;
    // FNV-1a 64-bit hash → 16 hex chars
    uint64_t hash_value = 14695981039346656037ULL;
    for (unsigned char byte_val : combined) {
        hash_value ^= byte_val;
        hash_value *= 1099511628211ULL;
    }
    wchar_t hex_buf[32];
    swprintf(hex_buf, 32, L"%016llX.imc", (unsigned long long)hash_value);
    return hex_buf;
}

static void cache_write_file(const std::wstring& path, const std::string& key,
                             const std::string& data) {
    std::string encrypted = xor_crypt(data, key);
    HANDLE fh = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(fh, encrypted.data(), (DWORD)encrypted.size(), &written, nullptr);
    CloseHandle(fh);
}

static bool cache_read_file(const std::wstring& path, const std::string& key,
                            std::string& out_data) {
    HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    DWORD file_size = GetFileSize(fh, nullptr);
    if (file_size == 0 || file_size == INVALID_FILE_SIZE) { CloseHandle(fh); return false; }
    std::string encrypted(file_size, '\0');
    DWORD bytes_read = 0;
    ReadFile(fh, encrypted.data(), file_size, &bytes_read, nullptr);
    CloseHandle(fh);
    if (bytes_read != file_size) return false;
    out_data = xor_crypt(encrypted, key);
    return true;
}

static void cache_write(const std::string& folder, const std::string& uid,
                        const std::string& key, const std::string& body_text,
                        const std::string& raw_html = {}) {
    std::wstring dir = get_cache_dir();
    if (dir.empty()) return;
    std::wstring base = dir + L"\\" + cache_filename(folder, uid);
    cache_write_file(base, key, body_text);
    if (!raw_html.empty())
        cache_write_file(base + L".h", key, raw_html);
}

// Fast check whether an email's body is already cached (no decryption needed).
static bool cache_exists(const std::string& folder, const std::string& uid) {
    std::wstring dir = get_cache_dir();
    if (dir.empty()) return false;
    std::wstring path = dir + L"\\" + cache_filename(folder, uid);
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool cache_read(const std::string& folder, const std::string& uid,
                       const std::string& key, std::string& out_body,
                       std::string& out_html) {
    std::wstring dir = get_cache_dir();
    if (dir.empty()) return false;
    std::wstring path = dir + L"\\" + cache_filename(folder, uid);
    HANDLE file_handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) return false;
    DWORD file_size = GetFileSize(file_handle, nullptr);
    if (file_size == 0 || file_size == INVALID_FILE_SIZE) { CloseHandle(file_handle); return false; }
    std::string encrypted(file_size, '\0');
    DWORD bytes_read = 0;
    ReadFile(file_handle, encrypted.data(), file_size, &bytes_read, nullptr);
    CloseHandle(file_handle);
    if (bytes_read != file_size) return false;
    out_body = xor_crypt(encrypted, key);
    // Try to load companion HTML cache
    cache_read_file(path + L".h", key, out_html);
    return true;
}

// ─── Account configuration ────────────────────────────────────────────────────
struct AccountConfig {
    std::string provider;      // "icloud" | "gmail" | "custom"
    std::string display_name;
    std::string email;
    std::string password;
    std::string imap_host;
    int         imap_port = 993;
    std::string smtp_host;
    int         smtp_port = 587;
};

static const AccountConfig PRESET_ICLOUD = {
    "icloud", "iCloud", "", "",
    "imap.mail.me.com", 993, "smtp.mail.me.com", 587
};
static const AccountConfig PRESET_GMAIL = {
    "gmail", "Gmail", "", "",
    "imap.gmail.com", 993, "smtp.gmail.com", 587
};
static const AccountConfig PRESET_OUTLOOK = {
    "outlook", "Outlook", "", "",
    "outlook.office365.com", 993, "smtp-mail.outlook.com", 587
};
static const AccountConfig PRESET_EXCHANGE = {
    "exchange", "Exchange", "", "",
    "mail.company.com", 993, "mail.company.com", 587
};

static AccountConfig g_account;              // active account config
static std::vector<AccountConfig> g_accounts; // all configured accounts
static int g_active_account_index = 0;        // which account is active

// ─── Mail structures ─────────────────────────────────────────────────────────
struct MailHeader {
    std::string uid;
    std::string from_address;
    std::string subject_text;
    std::string date_string;
    bool is_unread = true;
};

struct AttachmentInfo {
    std::string filename;
    std::string content_type;
    std::string raw_bytes;     // empty until fetched on demand
    std::string section_number; // IMAP section e.g. "2" or "1.2"
    size_t      declared_size = 0; // bytes from BODYSTRUCTURE
    std::string folder_name;   // needed for on-demand fetch
    std::string uid;           // needed for on-demand fetch
    std::string encoding;      // transfer encoding e.g. "base64"
};

struct BodyResult {
    std::string text;
    std::string html_raw;
    std::vector<AttachmentInfo> attachments;
};

struct ComposeFields {
    std::wstring to_addresses;
    std::wstring cc_addresses;
    std::wstring subject_text;
    std::wstring body_text;
    std::wstring from_address;
};

// ─── Attachment metadata cache (.att files) ───────────────────────────────────

// Write attachment list to <hash>.att cache file (XOR encrypted).
// An empty vector writes a valid file so we know "no attachments" vs "not cached".
static void cache_write_attachments(const std::string& folder,
                                    const std::string& uid,
                                    const std::string& key,
                                    const std::vector<AttachmentInfo>& atts) {
    std::wstring dir = get_cache_dir();
    if (dir.empty()) return;
    std::wstring path = dir + L"\\" + cache_filename(folder, uid) + L".att";

    std::string buf;
    auto write_u32 = [&](uint32_t value) {
        buf.append((char*)&value, 4);
    };
    auto write_u64 = [&](uint64_t value) {
        buf.append((char*)&value, 8);
    };
    auto write_str = [&](const std::string& str) {
        uint16_t len = (uint16_t)std::min(str.size(), (size_t)65535);
        buf.append((char*)&len, 2);
        buf.append(str.data(), len);
    };

    write_u32(1);                           // version
    write_u32((uint32_t)atts.size());
    for (const auto& att : atts) {
        write_str(att.filename);
        write_str(att.content_type);
        write_str(att.section_number);
        write_str(att.encoding);
        write_str(att.folder_name);
        write_str(att.uid);
        write_u64((uint64_t)att.declared_size);
    }

    cache_write_file(path, key, buf);
}

// Read attachment list from <hash>.att cache. Returns true if file existed
// (even if empty = no attachments). Returns false if not yet cached.
static bool cache_read_attachments(const std::string& folder,
                                   const std::string& uid,
                                   const std::string& key,
                                   std::vector<AttachmentInfo>& out_atts) {
    std::wstring dir = get_cache_dir();
    if (dir.empty()) return false;
    std::wstring path = dir + L"\\" + cache_filename(folder, uid) + L".att";

    std::string buf;
    if (!cache_read_file(path, key, buf)) return false;

    size_t pos = 0;
    auto read_u32 = [&]() -> uint32_t {
        if (pos + 4 > buf.size()) return 0;
        uint32_t value; memcpy(&value, buf.data() + pos, 4); pos += 4; return value;
    };
    auto read_u64 = [&]() -> uint64_t {
        if (pos + 8 > buf.size()) return 0;
        uint64_t value; memcpy(&value, buf.data() + pos, 8); pos += 8; return value;
    };
    auto read_str = [&]() -> std::string {
        if (pos + 2 > buf.size()) return {};
        uint16_t len; memcpy(&len, buf.data() + pos, 2); pos += 2;
        if (pos + len > buf.size()) return {};
        std::string str(buf.data() + pos, len); pos += len; return str;
    };

    uint32_t version = read_u32();
    if (version != 1) return false;
    uint32_t count = read_u32();
    for (uint32_t att_index = 0; att_index < count && pos < buf.size(); ++att_index) {
        AttachmentInfo att;
        att.filename       = read_str();
        att.content_type   = read_str();
        att.section_number = read_str();
        att.encoding       = read_str();
        att.folder_name    = read_str();
        att.uid            = read_str();
        att.declared_size  = (size_t)read_u64();
        out_atts.push_back(std::move(att));
    }
    return true;
}

// ─── Header index cache (_hdr files) ─────────────────────────────────────────

// Return path to the folder-level header cache file.
static std::wstring header_cache_path(const std::string& folder) {
    uint64_t hash_value = 14695981039346656037ULL;
    for (unsigned char folder_char : folder) {
        hash_value ^= folder_char;
        hash_value *= 1099511628211ULL;
    }
    wchar_t hex_buf[32];
    swprintf(hex_buf, 32, L"%016llX_hdr", (unsigned long long)hash_value);
    std::wstring dir = get_cache_dir();
    return dir.empty() ? L"" : dir + L"\\" + hex_buf;
}

static void cache_write_headers(const std::string& folder, const std::string& key,
                                const std::vector<MailHeader>& headers) {
    std::wstring path = header_cache_path(folder);
    if (path.empty()) return;
    std::string buf;
    auto write_u32 = [&](uint32_t value) { buf.append((char*)&value, 4); };
    auto write_str = [&](const std::string& str) {
        uint16_t len = (uint16_t)std::min(str.size(), (size_t)65535);
        buf.append((char*)&len, 2);
        buf.append(str.data(), len);
    };
    write_u32(1);  // version
    write_u32((uint32_t)headers.size());
    for (const auto& header : headers) {
        write_str(header.uid);
        write_str(header.from_address);
        write_str(header.subject_text);
        write_str(header.date_string);
        uint8_t flags = header.is_unread ? 1 : 0;
        buf.push_back((char)flags);
    }
    cache_write_file(path, key, buf);
}

static bool cache_read_headers(const std::string& folder, const std::string& key,
                               std::vector<MailHeader>& out_headers) {
    std::wstring path = header_cache_path(folder);
    if (path.empty()) return false;
    std::string buf;
    if (!cache_read_file(path, key, buf)) return false;
    size_t pos = 0;
    auto read_u32 = [&]() -> uint32_t {
        if (pos + 4 > buf.size()) return 0;
        uint32_t value; memcpy(&value, buf.data() + pos, 4); pos += 4; return value;
    };
    auto read_str = [&]() -> std::string {
        if (pos + 2 > buf.size()) return {};
        uint16_t len; memcpy(&len, buf.data() + pos, 2); pos += 2;
        if (pos + len > buf.size()) return {};
        std::string str(buf.data() + pos, len); pos += len; return str;
    };
    if (read_u32() != 1) return false;
    uint32_t count = read_u32();
    out_headers.reserve(count);
    for (uint32_t header_index = 0; header_index < count && pos < buf.size(); ++header_index) {
        MailHeader header;
        header.uid           = read_str();
        header.from_address  = read_str();
        header.subject_text  = read_str();
        header.date_string   = read_str();
        header.is_unread     = (pos < buf.size() && ((uint8_t)buf[pos++] & 1));
        out_headers.push_back(std::move(header));
    }
    return !out_headers.empty();
}

// ─── Account config file I/O ─────────────────────────────────────────────────
static std::wstring get_account_config_path() {
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring path = exe_path;
    size_t sep = path.rfind(L'\\');
    if (sep != std::wstring::npos) path.resize(sep);
    return path + L"\\account.cfg";
}

static void save_account_config(const AccountConfig& cfg) {
    std::wstring path = get_account_config_path();
    std::string data;
    data += cfg.provider + "\n";
    data += cfg.display_name + "\n";
    data += cfg.email + "\n";
    data += cfg.password + "\n";
    data += cfg.imap_host + "\n";
    data += std::to_string(cfg.imap_port) + "\n";
    data += cfg.smtp_host + "\n";
    data += std::to_string(cfg.smtp_port) + "\n";
    std::string key = "FastMailAccountKey2024";
    std::string encrypted = xor_crypt(data, key);
    HANDLE fh = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(fh, encrypted.data(), (DWORD)encrypted.size(), &written, nullptr);
    CloseHandle(fh);
}

static bool load_account_config(AccountConfig& cfg) {
    std::wstring path = get_account_config_path();
    HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    DWORD file_size = GetFileSize(fh, nullptr);
    if (file_size == 0 || file_size == INVALID_FILE_SIZE) { CloseHandle(fh); return false; }
    std::string encrypted(file_size, '\0');
    DWORD bytes_read = 0;
    ReadFile(fh, encrypted.data(), file_size, &bytes_read, nullptr);
    CloseHandle(fh);
    if (bytes_read != file_size) return false;
    std::string key = "FastMailAccountKey2024";
    std::string data = xor_crypt(encrypted, key);
    std::istringstream ss(data);
    std::string line;
    auto next_line = [&]() -> std::string {
        return std::getline(ss, line) ? line : "";
    };
    cfg.provider      = next_line();
    cfg.display_name  = next_line();
    cfg.email         = next_line();
    cfg.password      = next_line();
    cfg.imap_host     = next_line();
    std::string imap_port_str = next_line();
    cfg.imap_port     = imap_port_str.empty() ? 993 : std::stoi(imap_port_str);
    cfg.smtp_host     = next_line();
    std::string smtp_port_str = next_line();
    cfg.smtp_port     = smtp_port_str.empty() ? 587 : std::stoi(smtp_port_str);
    return !cfg.email.empty() && !cfg.imap_host.empty();
}

// ─── Multi-account config file I/O ───────────────────────────────────────────
static std::wstring get_all_accounts_path() {
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring path = exe_path;
    size_t sep = path.rfind(L'\\');
    if (sep != std::wstring::npos) path.resize(sep);
    return path + L"\\accounts.cfg";
}

static void save_all_accounts(const std::vector<AccountConfig>& accounts) {
    std::wstring path = get_all_accounts_path();
    std::string data;
    data += std::to_string(accounts.size()) + "\n";
    for (const auto& cfg : accounts) {
        data += cfg.provider + "\n" + cfg.display_name + "\n" + cfg.email + "\n"
             + cfg.password + "\n" + cfg.imap_host + "\n"
             + std::to_string(cfg.imap_port) + "\n"
             + cfg.smtp_host + "\n" + std::to_string(cfg.smtp_port) + "\n";
    }
    std::string key = "FastMailAccountKey2024";
    std::string enc = xor_crypt(data, key);
    HANDLE fh = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return;
    DWORD written_bytes;
    WriteFile(fh, enc.data(), (DWORD)enc.size(), &written_bytes, nullptr);
    CloseHandle(fh);
}

static bool load_all_accounts(std::vector<AccountConfig>& accounts) {
    std::wstring path = get_all_accounts_path();
    HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    DWORD file_size = GetFileSize(fh, nullptr);
    if (!file_size || file_size == INVALID_FILE_SIZE) { CloseHandle(fh); return false; }
    std::string enc(file_size, '\0');
    DWORD bytes_read = 0;
    ReadFile(fh, enc.data(), file_size, &bytes_read, nullptr);
    CloseHandle(fh);
    if (bytes_read != file_size) return false;
    std::string data = xor_crypt(enc, "FastMailAccountKey2024");
    std::istringstream ss(data);
    std::string line;
    auto next_line = [&]() -> std::string {
        return std::getline(ss, line) ? line : std::string{};
    };
    int count = 0;
    try { count = std::stoi(next_line()); } catch (...) { return false; }
    accounts.clear();
    for (int index = 0; index < count; ++index) {
        AccountConfig cfg;
        cfg.provider     = next_line();
        cfg.display_name = next_line();
        cfg.email        = next_line();
        cfg.password     = next_line();
        cfg.imap_host    = next_line();
        std::string imap_port_str = next_line();
        cfg.imap_port    = imap_port_str.empty() ? 993 : std::stoi(imap_port_str);
        cfg.smtp_host    = next_line();
        std::string smtp_port_str = next_line();
        cfg.smtp_port    = smtp_port_str.empty() ? 587 : std::stoi(smtp_port_str);
        if (!cfg.email.empty()) accounts.push_back(cfg);
    }
    return !accounts.empty();
}

// ─── Provider icon drawing ────────────────────────────────────────────────────
// Draw iCloud-style cloud icon (blue background with white cloud) on dc at (x,y) size sz
static void draw_icloud_icon(HDC dc, int icon_x, int icon_y, int icon_sz) {
    HPEN none_pen  = CreatePen(PS_NULL, 0, 0);
    HBRUSH sky_br  = CreateSolidBrush(RGB(0, 122, 204));
    HBRUSH white_br = CreateSolidBrush(RGB(255, 255, 255));
    HPEN old_pen   = (HPEN)SelectObject(dc, none_pen);
    HBRUSH old_br  = (HBRUSH)SelectObject(dc, sky_br);

    // Blue background circle
    Ellipse(dc, icon_x, icon_y, icon_x + icon_sz, icon_y + icon_sz);

    // White cloud puffs
    SelectObject(dc, white_br);
    int cx = icon_x + icon_sz / 2;
    int cy = icon_y + icon_sz * 55 / 100;
    int radius_left  = icon_sz / 6;
    int radius_center = icon_sz / 5;
    int radius_right = icon_sz / 6;
    int puff_offset  = icon_sz / 4;
    Ellipse(dc, cx - puff_offset - radius_left,  cy - radius_left,
                cx - puff_offset + radius_left,  cy + radius_left);
    Ellipse(dc, cx - radius_center, cy - radius_center,
                cx + radius_center, cy + radius_center * 3 / 4);
    Ellipse(dc, cx + puff_offset - radius_right, cy - radius_right,
                cx + puff_offset + radius_right, cy + radius_right);
    RECT cloud_base = { cx - puff_offset - radius_left, cy,
                        cx + puff_offset + radius_right, cy + radius_left };
    FillRect(dc, &cloud_base, white_br);

    SelectObject(dc, old_pen);
    SelectObject(dc, old_br);
    DeleteObject(none_pen);
    DeleteObject(sky_br);
    DeleteObject(white_br);
}

// Draw Gmail-style G icon (red circle with white "G") on dc at (x,y) size sz
static void draw_gmail_icon(HDC dc, int icon_x, int icon_y, int icon_sz) {
    HPEN none_pen  = CreatePen(PS_NULL, 0, 0);
    HBRUSH red_br  = CreateSolidBrush(RGB(219, 68, 55));
    HPEN old_pen   = (HPEN)SelectObject(dc, none_pen);
    HBRUSH old_br  = (HBRUSH)SelectObject(dc, red_br);
    Ellipse(dc, icon_x, icon_y, icon_x + icon_sz, icon_y + icon_sz);

    HFONT letter_font = CreateFontW(-icon_sz * 55 / 100, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
    HFONT old_font = (HFONT)SelectObject(dc, letter_font);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    RECT text_rc = { icon_x, icon_y, icon_x + icon_sz, icon_y + icon_sz };
    DrawTextW(dc, L"G", -1, &text_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_font);
    DeleteObject(letter_font);

    SelectObject(dc, old_pen);
    SelectObject(dc, old_br);
    DeleteObject(none_pen);
    DeleteObject(red_br);
}

static void draw_outlook_icon(HDC dc, int x, int y, int sz) {
    HBRUSH br = CreateSolidBrush(RGB(0, 114, 198));
    HPEN none = CreatePen(PS_NULL, 0, 0);
    HPEN old_pen = (HPEN)SelectObject(dc, none);
    HBRUSH old_br = (HBRUSH)SelectObject(dc, br);
    Ellipse(dc, x, y, x + sz, y + sz);
    SelectObject(dc, old_pen); SelectObject(dc, old_br);
    DeleteObject(br); DeleteObject(none);
    HFONT font = CreateFontW(-sz * 50 / 100, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
    HFONT old_f = (HFONT)SelectObject(dc, font);
    SetTextColor(dc, RGB(255,255,255)); SetBkMode(dc, TRANSPARENT);
    RECT r = {x, y, x+sz, y+sz};
    DrawTextW(dc, L"O", -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc, old_f); DeleteObject(font);
}
static void draw_exchange_icon(HDC dc, int x, int y, int sz) {
    HBRUSH br = CreateSolidBrush(RGB(0, 72, 147));
    HPEN none = CreatePen(PS_NULL, 0, 0);
    HPEN old_pen = (HPEN)SelectObject(dc, none);
    HBRUSH old_br = (HBRUSH)SelectObject(dc, br);
    Ellipse(dc, x, y, x + sz, y + sz);
    SelectObject(dc, old_pen); SelectObject(dc, old_br);
    DeleteObject(br); DeleteObject(none);
    HFONT font = CreateFontW(-sz * 50 / 100, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
    HFONT old_f = (HFONT)SelectObject(dc, font);
    SetTextColor(dc, RGB(255,255,255)); SetBkMode(dc, TRANSPARENT);
    RECT r = {x, y, x+sz, y+sz};
    DrawTextW(dc, L"Ex", -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc, old_f); DeleteObject(font);
}

// ─── Account settings window IDs ─────────────────────────────────────────────
#define IDC_PROV_ICLOUD     5001
#define IDC_PROV_GMAIL      5002
#define IDC_PROV_CUSTOM     5003
#define IDC_PROV_OUTLOOK    5004
#define IDC_PROV_EXCHANGE   5005
#define IDC_SETTINGS_EMAIL  5010
#define IDC_SETTINGS_PASS   5011
#define IDC_SETTINGS_HOST   5012
#define IDC_SETTINGS_PORT   5013
#define IDC_SETTINGS_SMTP   5014
#define IDC_SETTINGS_SPORT  5015
#define IDC_SETTINGS_TEST   5016
#define IDC_SETTINGS_SAVE   5017
#define IDC_SETTINGS_CANCEL 5018

struct SettingsState {
    AccountConfig config;
    bool ok       = false;
    int  edit_index = -1;  // -1 = adding new; >=0 = editing g_accounts[edit_index]
};
static SettingsState g_settings_state;

// settings_wnd_proc and show_account_settings are defined after ImapClient/SmtpClient
// (they need ImapClient for the "Test Connection" feature).
static LRESULT CALLBACK settings_wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
static bool show_account_settings(HWND parent, int edit_index = -1);

// ─── IMAP Client ─────────────────────────────────────────────────────────────
class ImapClient {
public:
    ImapClient() : tag_counter_(1), connected_(false) {}
    ~ImapClient() { disconnect(); }

    std::string last_error;

    bool connect(const std::string& email_address, const std::string& app_password) {
        try {
            debug_log("IMAP: connect() called for " + email_address);
            email_address_ = email_address;
            app_password_  = app_password;
            connection_    = std::make_unique<TlsConnection>();
            const char* imap_host = g_account.imap_host.empty()
                ? IMAP_HOST : g_account.imap_host.c_str();
            int imap_port = g_account.imap_port > 0
                ? g_account.imap_port : IMAP_PORT;
            debug_log("IMAP: connecting TCP+TLS to " + std::string(imap_host));
            if (!connection_->connect_direct(imap_host, imap_port)) {
                last_error = "Cannot connect to IMAP server";
                debug_log("IMAP: connect_direct FAILED: " + last_error);
                return false;
            }
            debug_log("IMAP: TLS connected, reading greeting");
            // Read server greeting
            std::string greeting = connection_->recv_line();
            debug_log("IMAP: greeting=" + greeting);
            if (greeting.find("* OK") == std::string::npos) {
                last_error = "Bad greeting: " + greeting;
                return false;
            }
            // LOGIN
            debug_log("IMAP: sending LOGIN");
            std::string login_tag = next_tag();
            std::string login_cmd = login_tag + " LOGIN " + quote_imap(email_address) + " " + quote_imap(app_password) + "\r\n";
            connection_->send_data(login_cmd);
            debug_log("IMAP: reading LOGIN response");
            std::string login_response = read_until_tag(login_tag);
            debug_log("IMAP: LOGIN response=" + login_response.substr(0, 120));
            if (login_response.find(login_tag + " OK") == std::string::npos) {
                last_error = "LOGIN failed";
                return false;
            }
            connected_ = true;
            debug_log("IMAP: connected and authenticated");
            return true;
        } catch (const std::exception& exception_error) {
            last_error = std::string("Exception in connect: ") + exception_error.what();
            debug_log("IMAP: exception: " + last_error);
            return false;
        } catch (...) {
            last_error = "Unexpected exception in connect";
            debug_log("IMAP: unknown exception");
            return false;
        }
    }

    void disconnect() {
        if (connection_) {
            if (connected_) {
                std::string logout_tag = next_tag();
                connection_->send_data(logout_tag + " LOGOUT\r\n");
            }
            connection_->close();
            connection_.reset();
        }
        connected_ = false;
    }

    bool is_connected() const { return connected_; }

    std::vector<std::string> list_folders() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> folders;
        std::string list_tag = next_tag();
        connection_->send_data(list_tag + " LIST \"\" \"*\"\r\n");
        std::string list_response = read_until_tag(list_tag);
        // Parse each "* LIST (\Flags) "/" "FolderName" line
        std::istringstream stream(list_response);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.find("* LIST") == std::string::npos) continue;
            // Find the folder name after the last separator
            size_t last_quote = line.rfind('"');
            if (last_quote != std::string::npos) {
                size_t prev_quote = line.rfind('"', last_quote - 1);
                if (prev_quote != std::string::npos && prev_quote < last_quote) {
                    folders.push_back(line.substr(prev_quote + 1, last_quote - prev_quote - 1));
                    continue;
                }
            }
            // Unquoted name
            std::istringstream line_stream(line);
            std::string token;
            // skip "* LIST" then flags then separator then name
            line_stream >> token >> token; // * LIST
            // skip to after NIL or separator
            std::string remaining;
            std::getline(line_stream, remaining);
            // Find last non-space token
            size_t name_start = remaining.rfind(' ');
            if (name_start != std::string::npos) {
                std::string folder_name = trim_string(remaining.substr(name_start));
                if (!folder_name.empty()) folders.push_back(folder_name);
            }
        }
        return folders;
    }

    bool select_folder(const std::string& folder_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_folder_ = folder_name;
        std::string select_tag = next_tag();
        connection_->send_data(select_tag + " SELECT " + quote_imap(folder_name) + "\r\n");
        std::string response = read_until_tag(select_tag);
        return response.find(select_tag + " OK") != std::string::npos;
    }

    // Must be called while mutex_ is held by caller
    std::vector<std::string> search_all_uids() {
        std::vector<std::string> uids;
        std::string search_tag = next_tag();
        connection_->send_data(search_tag + " UID SEARCH ALL\r\n");
        std::string response = read_until_tag(search_tag);
        size_t search_pos = response.find("* SEARCH");
        if (search_pos == std::string::npos) return uids;
        size_t line_end = response.find('\n', search_pos);
        std::string search_line = response.substr(search_pos + 8, line_end - search_pos - 8);
        std::istringstream stream(search_line);
        std::string uid_str;
        while (stream >> uid_str) {
            if (!uid_str.empty() && isdigit((unsigned char)uid_str[0]))
                uids.push_back(uid_str);
        }
        std::reverse(uids.begin(), uids.end()); // newest first
        return uids;
    }

    // Public: SELECT folder + SEARCH ALL in one locked transaction → UIDs newest first
    std::vector<std::string> select_and_get_uids(const std::string& folder_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_folder_ = folder_name;
        std::string select_tag = next_tag();
        connection_->send_data(select_tag + " SELECT " + quote_imap(folder_name) + "\r\n");
        std::string response = read_until_tag(select_tag);
        if (response.find(select_tag + " OK") == std::string::npos) return {};
        return search_all_uids();
    }

    // Public: fetch headers for an explicit set of UIDs (one IMAP round trip)
    std::vector<MailHeader> fetch_headers_batch(const std::vector<std::string>& uids) {
        if (uids.empty()) return {};
        std::lock_guard<std::mutex> lock(mutex_);
        std::string uid_set;
        for (size_t idx = 0; idx < uids.size(); ++idx) {
            if (idx > 0) uid_set += ',';
            uid_set += uids[idx];
        }
        std::string fetch_tag = next_tag();
        connection_->send_data(fetch_tag + " UID FETCH " + uid_set +
            " (FLAGS BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)])\r\n");
        return parse_fetch_response(read_until_tag(fetch_tag));
    }

    std::vector<MailHeader> fetch_headers(const std::vector<std::string>& uids) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MailHeader> headers;
        for (const std::string& uid : uids) {
            std::string fetch_tag = next_tag();
            connection_->send_data(fetch_tag + " UID FETCH " + uid +
                " (BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)])\r\n");
            std::string fetch_response = read_until_tag(fetch_tag);
            MailHeader header;
            header.uid = uid;
            // Parse header fields
            std::istringstream stream(fetch_response);
            std::string line;
            while (std::getline(stream, line)) {
                std::string lower_line = to_lower(line);
                if (lower_line.find("from:") == 0) {
                    header.from_address = decode_rfc2047(trim_string(line.substr(5)));
                } else if (lower_line.find("subject:") == 0) {
                    header.subject_text = decode_rfc2047(trim_string(line.substr(8)));
                } else if (lower_line.find("date:") == 0) {
                    header.date_string = trim_string(line.substr(5));
                }
            }
            headers.push_back(header);
        }
        return headers;
    }

    // Kept for compatibility — delegates to new batched methods
    std::vector<MailHeader> fetch_folder_headers(const std::string& folder_name) {
        auto uids = select_and_get_uids(folder_name);
        return fetch_headers_batch(uids);
    }

    std::string fetch_body(const std::string& folder_name, const std::string& uid) {
        {
            std::string cached_text, cached_html;
            if (cache_read(folder_name, uid, email_address_, cached_text, cached_html)) {
                last_attachments_.clear();
                raw_html_ = std::move(cached_html);
                cid_images_.clear();
                debug_log("CACHE: hit for uid=" + uid);
                return cached_text;
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        // Re-SELECT the folder in case load_folder switched it since we loaded headers
        if (current_folder_ != folder_name) {
            std::string select_tag = next_tag();
            connection_->send_data(select_tag + " SELECT " + quote_imap(folder_name) + "\r\n");
            read_until_tag(select_tag);
            current_folder_ = folder_name;
        }
        std::string fetch_tag = next_tag();
        connection_->send_data(fetch_tag + " UID FETCH " + uid + " (BODY.PEEK[])\r\n");
        std::string raw_response = read_until_tag(fetch_tag);
        std::string body_text = parse_mime_body(raw_response);
        if (!body_text.empty())
            cache_write(folder_name, uid, email_address_, body_text, raw_html_);
        return body_text;
    }

    // ── BODYSTRUCTURE-based smart fetch ──────────────────────────────────────

    // prefetch_mode=true: runs in background thread — uses local vars, never
    // touches raw_html_/last_attachments_/cid_images_ so the UI thread's
    // results are never corrupted by the concurrent prefetch.
    std::string fetch_body_smart(const std::string& folder_name, const std::string& uid,
                                 bool prefetch_mode = false) {
        std::string cached_text, cached_html;
        bool cache_hit = cache_read(folder_name, uid, email_address_, cached_text, cached_html);

        // Fast path for interactive opens: if both body and attachment metadata are
        // cached on disk, return immediately with zero IMAP calls.
        if (cache_hit && !prefetch_mode) {
            std::vector<AttachmentInfo> cached_atts;
            bool att_cached = cache_read_attachments(folder_name, uid, email_address_, cached_atts);
            if (att_cached) {
                raw_html_         = std::move(cached_html);
                cid_images_.clear();
                last_attachments_ = std::move(cached_atts);
                debug_log("CACHE: zero-IMAP hit uid=" + uid +
                          " atts=" + std::to_string(last_attachments_.size()) +
                          " html=" + std::to_string(raw_html_.size()));
                return cached_text;
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);

        // Only reset shared output state for interactive (non-prefetch) fetches.
        if (!prefetch_mode) {
            raw_html_.clear();
            cid_images_.clear();
            last_attachments_.clear();
        }

        // Local HTML / CID / attachment state used in prefetch mode so we never
        // overwrite the member variables that the UI thread is about to consume.
        std::string        local_raw_html;
        std::map<std::string, std::string> local_cid_images;
        std::vector<AttachmentInfo>        local_attachments;

        auto& out_html  = prefetch_mode ? local_raw_html   : raw_html_;
        auto& out_cids  = prefetch_mode ? local_cid_images : cid_images_;
        auto& out_atts  = prefetch_mode ? local_attachments: last_attachments_;

        auto ensure_folder = [&]() {
            if (current_folder_ != folder_name) {
                std::string tag = next_tag();
                connection_->send_data(tag + " SELECT " + quote_imap(folder_name) + "\r\n");
                read_until_tag(tag);
                current_folder_ = folder_name;
            }
        };

        ensure_folder();

        // Step 1: Fetch BODYSTRUCTURE (always — needed for attachment metadata)
        std::string bs_tag = next_tag();
        connection_->send_data(bs_tag + " UID FETCH " + uid + " (BODYSTRUCTURE)\r\n");
        std::string bs_response = read_until_tag(bs_tag);
        debug_log("BODYSTRUCTURE: " + bs_response.substr(0, 300));

        // Extract BODYSTRUCTURE sexp from response
        std::vector<PartInfo> parts;
        {
            std::string bs_lower = to_lower(bs_response);
            size_t bs_start = bs_lower.find("bodystructure (");
            if (bs_start == std::string::npos) bs_start = bs_lower.find("bodystructure(");
            if (bs_start != std::string::npos) {
                size_t paren_pos = bs_response.find('(', bs_start);
                if (paren_pos != std::string::npos) {
                    SexprReader reader{bs_response, paren_pos + 1};
                    parse_bodystructure_part(reader, "", 1, parts);
                }
            }
        }

        debug_log("BODYSTRUCTURE parts: " + std::to_string(parts.size()));
        for (const auto& part : parts)
            debug_log("  part=" + part.section + " type=" + part.type + "/" + part.subtype +
                      " size=" + std::to_string(part.declared_size) +
                      " attach=" + std::to_string(part.is_attachment) +
                      " file=" + part.filename);

        // If BODYSTRUCTURE response is empty the connection was dropped by the server.
        // Mark as disconnected so the caller can reconnect before the next attempt.
        if (bs_response.empty() || bs_response.size() < 10) {
            debug_log("BODYSTRUCTURE: empty response — connection lost, marking disconnected");
            connected_ = false;
            return {};
        }

        // If we couldn't parse BODYSTRUCTURE, fall back to full fetch
        if (parts.empty()) {
            std::string fetch_tag = next_tag();
            connection_->send_data(fetch_tag + " UID FETCH " + uid + " (BODY.PEEK[])\r\n");
            std::string raw_response = read_until_tag(fetch_tag);
            std::string body_text = parse_mime_body(raw_response);
            if (!body_text.empty()) {
                if (!prefetch_mode)
                    cache_write_attachments(folder_name, uid, email_address_, last_attachments_);
                cache_write(folder_name, uid, email_address_, body_text);
            }
            return body_text;
        }

        // Step 2: Collect attachment metadata. Parts with Content-ID are inline images,
        // not listed as downloadable attachments.
        for (const auto& part : parts) {
            if (!part.is_attachment) continue;
            if (!part.content_id.empty()) continue;  // inline CID image — handled below
            AttachmentInfo att;
            att.filename       = part.filename.empty() ? ("attachment." + part.subtype) : part.filename;
            att.filename       = decode_rfc2047(att.filename);
            att.content_type   = part.type + "/" + part.subtype;
            att.section_number = part.section;
            att.declared_size  = part.declared_size;
            att.folder_name    = folder_name;
            att.uid            = uid;
            att.encoding       = part.encoding;
            out_atts.push_back(std::move(att));
        }

        // Step 2b: Fetch inline CID images and store as data URIs
        auto fetch_part_literal = [&](const std::string& section) -> std::string {
            std::string tag = next_tag();
            connection_->send_data(tag + " UID FETCH " + uid +
                                   " (BODY.PEEK[" + section + "])\r\n");
            std::string response = read_until_tag(tag);
            size_t brace = response.find('{');
            if (brace == std::string::npos) return {};
            size_t brace_end = response.find('}', brace + 1);
            if (brace_end == std::string::npos) return {};
            size_t lit_size = (size_t)std::stoull(response.substr(brace + 1, brace_end - brace - 1));
            size_t nl = response.find('\n', brace_end);
            if (nl == std::string::npos) return {};
            size_t start = nl + 1;
            size_t avail = response.size() > start ? response.size() - start : 0;
            return response.substr(start, std::min(lit_size, avail));
        };

        for (const auto& part : parts) {
            if (part.content_id.empty()) continue;
            if (part.type == "text") continue;  // skip text/* with CID (rare)
            std::string raw_data = fetch_part_literal(part.section);
            if (raw_data.empty()) continue;
            std::string b64;
            if (part.encoding == "base64") {
                for (char ch : raw_data) if (ch != '\r' && ch != '\n') b64 += ch;
            } else {
                std::string decoded = (part.encoding == "quoted-printable")
                    ? decode_quoted_printable(raw_data) : raw_data;
                b64 = encode_base64(decoded);
            }
            std::string mime_type = part.type + "/" + part.subtype;
            out_cids[part.content_id] = "data:" + mime_type + ";base64," + b64;
            debug_log("CID: loaded " + part.content_id + " size=" + std::to_string(b64.size()));
        }

        // Cache hit: body text is ready, attachment metadata is now populated — persist
        // attachment metadata so the next open uses the zero-IMAP fast path.
        if (cache_hit) {
            out_html = std::move(cached_html);
            debug_log("CACHE: hit uid=" + uid + " attachments=" +
                      std::to_string(out_atts.size()) +
                      " html=" + std::to_string(out_html.size()));
            cache_write_attachments(folder_name, uid, email_address_, out_atts);
            return cached_text;
        }

        // Step 3: Fetch text/html parts
        std::string plain_text;
        std::string html_text;

        for (const auto& part : parts) {
            if (part.is_attachment) continue;
            if (part.type != "text") continue;

            std::string section_tag = next_tag();
            std::string section_spec = "BODY.PEEK[" + part.section + "]";
            connection_->send_data(section_tag + " UID FETCH " + uid +
                                   " (" + section_spec + ")\r\n");
            std::string part_response = read_until_tag(section_tag);

            // Extract literal from response
            std::string part_data;
            size_t brace = part_response.find('{');
            if (brace != std::string::npos) {
                size_t brace_end = part_response.find('}', brace + 1);
                if (brace_end != std::string::npos) {
                    size_t part_literal_size = (size_t)std::stoull(
                        part_response.substr(brace + 1, brace_end - brace - 1));
                    size_t nl = part_response.find('\n', brace_end);
                    if (nl != std::string::npos) {
                        size_t part_start = nl + 1;
                        size_t part_avail = part_response.size() > part_start
                                            ? part_response.size() - part_start : 0;
                        part_data = part_response.substr(part_start,
                                                         std::min(part_literal_size, part_avail));
                    }
                }
            }
            if (part_data.empty()) part_data = part_response;

            // Decode transfer encoding
            std::string decoded;
            if (part.encoding == "base64") {
                std::string base64_data;
                for (char ch : part_data) if (ch != '\r' && ch != '\n') base64_data += ch;
                decoded = decode_base64(base64_data);
            } else if (part.encoding == "quoted-printable") {
                decoded = decode_quoted_printable(part_data);
            } else {
                decoded = part_data;
            }
            decoded = convert_charset_to_utf8(decoded, part.charset.empty() ? "utf-8" : part.charset);

            if (part.subtype == "html") {
                if (out_html.empty()) {
                    // Keep external https:// URLs as-is — MOTW in the temp HTML
                    // file lets IE load them from the internet zone. Embedding
                    // images as base64 bloats HTML to several MB and hangs the UI.
                    out_html = decoded;
                }
                if (html_text.empty()) html_text = clean_text_body(strip_html(decoded));
            } else {
                if (plain_text.empty()) plain_text = clean_text_body(decoded);
            }
        }

        // Replace cid: references in HTML with data URIs.
        // Search is case-insensitive; also try prefix match when the HTML appends
        // an @domain suffix to the CID (e.g. cid:uuid@domain.com vs stored key=uuid).
        if (!out_html.empty() && !out_cids.empty()) {
            // Debug: show where cid: appears in html
            {
                std::string hl = to_lower(out_html);
                size_t first_cid = hl.find("cid:");
                if (first_cid != std::string::npos) {
                    size_t end = std::min(first_cid + 80, out_html.size());
                    debug_log("CID in html at " + std::to_string(first_cid) +
                              ": [" + out_html.substr(first_cid, end - first_cid) + "]");
                } else {
                    debug_log("CID: no 'cid:' found in html (" +
                              std::to_string(out_html.size()) + " chars)");
                }
            }
            std::string html_lower = to_lower(out_html);
            for (const auto& cid_entry : out_cids) {
                std::string key_lower  = to_lower(cid_entry.first);
                std::string token      = "cid:" + key_lower;
                int replacements = 0;
                size_t search_pos = 0;
                while (search_pos < html_lower.size()) {
                    size_t found = html_lower.find(token, search_pos);
                    if (found == std::string::npos) break;
                    // Find end of CID value (terminated by " ' > space)
                    size_t val_end = out_html.find_first_of("\"' >", found + token.size());
                    if (val_end == std::string::npos) val_end = out_html.size();
                    size_t full_token_len = val_end - found;
                    out_html.replace(found, full_token_len, cid_entry.second);
                    html_lower.replace(found, full_token_len, cid_entry.second);
                    search_pos = found + cid_entry.second.size();
                    ++replacements;
                }
                debug_log("CID replace [" + cid_entry.first.substr(0, 40) + "] hits=" +
                          std::to_string(replacements));
            }
        }

        std::string result = plain_text.empty() ? html_text : plain_text;
        if (!result.empty()) {
            cache_write_attachments(folder_name, uid, email_address_, out_atts);
            cache_write(folder_name, uid, email_address_, result, out_html);
        }
        return result;
    }

    std::vector<unsigned char> fetch_attachment_section(const std::string& folder_name,
                                                         const std::string& uid,
                                                         const std::string& section,
                                                         const std::string& encoding) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_folder_ != folder_name) {
            std::string tag = next_tag();
            connection_->send_data(tag + " SELECT " + quote_imap(folder_name) + "\r\n");
            read_until_tag(tag);
            current_folder_ = folder_name;
        }
        std::string fetch_tag = next_tag();
        connection_->send_data(fetch_tag + " UID FETCH " + uid +
                               " (BODY.PEEK[" + section + "])\r\n");
        std::string response = read_until_tag(fetch_tag);

        // Extract literal
        std::string part_data;
        size_t brace = response.find('{');
        if (brace != std::string::npos) {
            size_t brace_end = response.find('}', brace);
            if (brace_end != std::string::npos) {
                size_t nl = response.find('\n', brace_end);
                if (nl != std::string::npos) part_data = response.substr(nl + 1);
            }
        }
        if (part_data.empty()) part_data = response;

        std::string decoded;
        std::string enc_lower = to_lower(encoding);
        if (enc_lower == "base64") {
            std::string base64_data;
            for (char ch : part_data) if (ch != '\r' && ch != '\n') base64_data += ch;
            decoded = decode_base64(base64_data);
        } else if (enc_lower == "quoted-printable") {
            decoded = decode_quoted_printable(part_data);
        } else {
            decoded = part_data;
        }

        return std::vector<unsigned char>(decoded.begin(), decoded.end());
    }

    std::vector<AttachmentInfo> take_last_attachments() {
        return std::move(last_attachments_);
    }

    std::string take_last_html() {
        return std::move(raw_html_);
    }

    bool mark_seen(const std::string& folder_name, const std::string& uid) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_folder_ != folder_name) {
            std::string select_tag = next_tag();
            connection_->send_data(select_tag + " SELECT " + quote_imap(folder_name) + "\r\n");
            read_until_tag(select_tag);
            current_folder_ = folder_name;
        }
        std::string store_tag = next_tag();
        connection_->send_data(store_tag + " UID STORE " + uid + " +FLAGS (\\Seen)\r\n");
        std::string response = read_until_tag(store_tag);
        return response.find(store_tag + " OK") != std::string::npos;
    }

    bool delete_message(const std::string& folder_name, const std::string& uid) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_folder_ != folder_name) {
            std::string select_tag = next_tag();
            connection_->send_data(select_tag + " SELECT " + quote_imap(folder_name) + "\r\n");
            read_until_tag(select_tag);
            current_folder_ = folder_name;
        }
        std::string store_tag = next_tag();
        connection_->send_data(store_tag + " UID STORE " + uid + " +FLAGS (\\Deleted)\r\n");
        read_until_tag(store_tag);
        std::string expunge_tag = next_tag();
        connection_->send_data(expunge_tag + " EXPUNGE\r\n");
        std::string response = read_until_tag(expunge_tag);
        return response.find(expunge_tag + " OK") != std::string::npos;
    }

    const std::string& get_email_address() const { return email_address_; }

    bool create_mailbox(const std::string& folder_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string tag = next_tag();
        connection_->send_data(tag + " CREATE " + quote_imap(folder_name) + "\r\n");
        std::string response = read_until_tag(tag);
        return response.find(tag + " OK") != std::string::npos;
    }

    bool delete_mailbox(const std::string& folder_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string tag = next_tag();
        connection_->send_data(tag + " DELETE " + quote_imap(folder_name) + "\r\n");
        std::string response = read_until_tag(tag);
        return response.find(tag + " OK") != std::string::npos;
    }

    bool rename_mailbox(const std::string& old_name, const std::string& new_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string tag = next_tag();
        connection_->send_data(tag + " RENAME " + quote_imap(old_name) +
                               " " + quote_imap(new_name) + "\r\n");
        std::string response = read_until_tag(tag);
        return response.find(tag + " OK") != std::string::npos;
    }

    bool move_messages(const std::string& source_folder,
                       const std::vector<std::string>& uids,
                       const std::string& dest_folder) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_folder_ != source_folder) {
            std::string tag = next_tag();
            connection_->send_data(tag + " SELECT " + quote_imap(source_folder) + "\r\n");
            read_until_tag(tag);
            current_folder_ = source_folder;
        }
        for (const auto& uid : uids) {
            // COPY to destination
            std::string copy_tag = next_tag();
            connection_->send_data(copy_tag + " UID COPY " + uid +
                                   " " + quote_imap(dest_folder) + "\r\n");
            std::string copy_resp = read_until_tag(copy_tag);
            if (copy_resp.find(copy_tag + " OK") == std::string::npos) continue;
            // Mark as deleted in source
            std::string store_tag = next_tag();
            connection_->send_data(store_tag + " UID STORE " + uid +
                                   " +FLAGS (\\Deleted)\r\n");
            read_until_tag(store_tag);
        }
        // Expunge deleted messages
        std::string exp_tag = next_tag();
        connection_->send_data(exp_tag + " EXPUNGE\r\n");
        read_until_tag(exp_tag);
        return true;
    }

private:
    std::unique_ptr<TlsConnection> connection_;
    std::mutex mutex_;
    std::string email_address_;
    std::string app_password_;
    std::string current_folder_;
    int  tag_counter_;
    bool connected_;
    std::vector<AttachmentInfo> last_attachments_;
    std::string raw_html_;
    std::map<std::string, std::string> cid_images_;  // content-id → data URI

    // ── S-expression reader for BODYSTRUCTURE ────────────────────────────────
    struct SexprReader {
        const std::string& src;
        size_t pos = 0;

        void skip_ws() {
            while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\r' || src[pos] == '\n'))
                ++pos;
        }

        // Returns next token. Special: "(" for open paren, ")" for close, "" for EOF.
        std::string next_token() {
            skip_ws();
            if (pos >= src.size()) return "";
            char ch = src[pos];
            if (ch == '(') { ++pos; return "("; }
            if (ch == ')') { ++pos; return ")"; }
            if (ch == '"') {
                ++pos;
                std::string tok;
                while (pos < src.size() && src[pos] != '"') {
                    if (src[pos] == '\\' && pos + 1 < src.size()) { ++pos; }
                    tok += src[pos++];
                }
                if (pos < src.size()) ++pos; // closing "
                return tok;
            }
            // NIL or atom
            std::string tok;
            while (pos < src.size() && src[pos] != ' ' && src[pos] != '(' &&
                   src[pos] != ')' && src[pos] != '\r' && src[pos] != '\n')
                tok += src[pos++];
            return tok;
        }

        // Skip one entire S-expression (atom or nested list)
        void skip_sexp() {
            skip_ws();
            if (pos >= src.size()) return;
            if (src[pos] == '(') {
                ++pos; // consume '('
                int depth = 1;
                while (pos < src.size() && depth > 0) {
                    if (src[pos] == '"') {
                        ++pos;
                        while (pos < src.size() && src[pos] != '"') {
                            if (src[pos] == '\\') ++pos;
                            ++pos;
                        }
                        if (pos < src.size()) ++pos;
                    } else if (src[pos] == '(') { ++depth; ++pos; }
                    else if (src[pos] == ')') { --depth; ++pos; }
                    else ++pos;
                }
            } else {
                next_token(); // skip atom
            }
        }
    };

    struct PartInfo {
        std::string section;       // "1", "2", "1.1", "1.2", etc.
        std::string type;          // "text", "application", etc.
        std::string subtype;       // "plain", "html", "pdf", etc.
        std::string encoding;      // "7bit", "base64", "quoted-printable"
        std::string charset;
        std::string filename;
        std::string content_id;    // Content-ID for CID inline images (without angle brackets)
        size_t      declared_size = 0;
        bool        is_attachment = false;
    };

    // Parse one BODYSTRUCTURE sexp — called after consuming the opening '('.
    static void parse_bodystructure_part(SexprReader& reader, const std::string& section_prefix,
                                         int part_index, std::vector<PartInfo>& out_parts) {
        (void)part_index;
        // Peek: if next char is '(', this is multipart
        reader.skip_ws();
        if (reader.pos < reader.src.size() && reader.src[reader.pos] == '(') {
            // Multipart: one or more child parts, then subtype string
            int child_index = 1;
            while (reader.pos < reader.src.size() && reader.src[reader.pos] == '(') {
                reader.next_token(); // consume opening '(' of child
                std::string child_section = section_prefix.empty()
                    ? std::to_string(child_index)
                    : section_prefix + "." + std::to_string(child_index);
                parse_bodystructure_part(reader, child_section, child_index, out_parts);
                ++child_index;
                // Consume the closing ')' of this child so the loop can see the next sibling's '('
                reader.skip_ws();
                if (reader.pos < reader.src.size() && reader.src[reader.pos] == ')')
                    reader.next_token();
            }
            // subtype and remaining fields — skip them
            while (reader.pos < reader.src.size() && reader.src[reader.pos] != ')') {
                reader.skip_sexp();
            }
            return;
        }

        // Leaf part: type subtype params body-id body-desc encoding size [lines] [extension]
        PartInfo part;
        part.section = section_prefix.empty() ? "1" : section_prefix;

        part.type    = to_lower(reader.next_token());
        part.subtype = to_lower(reader.next_token());

        // Parse parameter list (charset, name, etc.)
        reader.skip_ws();
        if (reader.pos < reader.src.size() && reader.src[reader.pos] == '(') {
            reader.next_token(); // consume '('
            while (reader.pos < reader.src.size() && reader.src[reader.pos] != ')') {
                std::string param_name  = to_lower(reader.next_token());
                std::string param_value = reader.next_token();
                if (param_name == "charset") part.charset  = to_lower(param_value);
                if (param_name == "name")    part.filename = param_value;
            }
            reader.next_token(); // consume ')'
        } else {
            reader.next_token(); // NIL
        }

        // body-id = Content-ID for inline images (CID references in HTML)
        {
            std::string raw_cid = reader.next_token();
            if (raw_cid != "NIL" && raw_cid != "nil" && !raw_cid.empty()) {
                if (raw_cid.front() == '<') raw_cid = raw_cid.substr(1);
                if (!raw_cid.empty() && raw_cid.back() == '>') raw_cid.pop_back();
                part.content_id = trim_string(raw_cid);
            }
        }
        reader.skip_sexp(); // body-description
        part.encoding = to_lower(reader.next_token());
        std::string size_str = reader.next_token();
        try {
            part.declared_size = size_str.empty() ? 0 : (size_t)std::stoull(size_str);
        } catch (...) {
            part.declared_size = 0;
        }

        // Skip optional line count for text parts
        if (part.type == "text") {
            reader.skip_ws();
            if (reader.pos < reader.src.size() && reader.src[reader.pos] != ')' &&
                reader.src[reader.pos] != '(') {
                reader.next_token(); // line count
            }
        }

        // Extension data: MD5, disposition, language — skip rest until closing ')'
        reader.skip_ws();
        while (reader.pos < reader.src.size() && reader.src[reader.pos] != ')') {
            reader.skip_ws();
            if (reader.pos < reader.src.size() && reader.src[reader.pos] == '(') {
                // Could be disposition list
                size_t saved = reader.pos;
                reader.next_token(); // consume '('
                std::string disposition = to_lower(reader.next_token());
                if (disposition == "attachment" || disposition == "inline") {
                    part.is_attachment = (disposition == "attachment");
                    reader.skip_ws();
                    if (reader.pos < reader.src.size() && reader.src[reader.pos] == '(') {
                        reader.next_token(); // '('
                        while (reader.pos < reader.src.size() && reader.src[reader.pos] != ')') {
                            std::string pname = to_lower(reader.next_token());
                            std::string pvalue = reader.next_token();
                            if ((pname == "filename" || pname == "filename*") && part.filename.empty())
                                part.filename = pvalue;
                        }
                        reader.next_token(); // ')'
                    }
                    reader.skip_ws();
                    if (reader.pos < reader.src.size() && reader.src[reader.pos] != ')')
                        reader.skip_sexp();
                    reader.skip_ws();
                    if (reader.pos < reader.src.size() && reader.src[reader.pos] == ')')
                        reader.next_token();
                } else {
                    reader.pos = saved;
                    reader.skip_sexp();
                }
            } else if (reader.pos < reader.src.size() && reader.src[reader.pos] != ')') {
                reader.skip_sexp();
            }
        }

        // Mark as attachment if non-text and has filename or binary type
        if (part.type != "text" && (part.is_attachment || !part.filename.empty() ||
            part.type == "application" || part.type == "image"))
            part.is_attachment = true;

        out_parts.push_back(part);
    }

    std::string next_tag() {
        char tag_buf[16];
        snprintf(tag_buf, sizeof(tag_buf), "A%04d", tag_counter_++);
        return tag_buf;
    }

    std::vector<MailHeader> parse_fetch_response(const std::string& response) {
        std::vector<MailHeader> headers;
        MailHeader current_header;
        bool in_fetch_block = false;
        std::istringstream stream(response);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.find("* ") == 0 && line.find(" FETCH ") != std::string::npos) {
                if (in_fetch_block && !current_header.uid.empty())
                    headers.push_back(current_header);
                current_header = MailHeader{};
                in_fetch_block = true;
                size_t uid_pos = line.find("UID ");
                if (uid_pos != std::string::npos) {
                    size_t uid_start = uid_pos + 4;
                    size_t uid_end = line.find_first_not_of("0123456789", uid_start);
                    if (uid_end == std::string::npos) uid_end = line.size();
                    current_header.uid = line.substr(uid_start, uid_end - uid_start);
                }
                // Parse \Seen flag to determine unread status
                size_t flags_pos = line.find("FLAGS (");
                if (flags_pos != std::string::npos) {
                    size_t flags_end = line.find(')', flags_pos + 7);
                    if (flags_end != std::string::npos) {
                        std::string flags_str = to_lower(line.substr(flags_pos + 7,
                                                                     flags_end - flags_pos - 7));
                        current_header.is_unread = (flags_str.find("\\seen") == std::string::npos);
                    }
                } else {
                    current_header.is_unread = true; // no FLAGS data → assume unread
                }
                continue;
            }
            if (!in_fetch_block) continue;
            std::string lower_line = to_lower(line);
            if (lower_line.find("from:") == 0)
                current_header.from_address = decode_rfc2047(trim_string(line.substr(5)));
            else if (lower_line.find("subject:") == 0)
                current_header.subject_text = decode_rfc2047(trim_string(line.substr(8)));
            else if (lower_line.find("date:") == 0)
                current_header.date_string = trim_string(line.substr(5));
        }
        if (in_fetch_block && !current_header.uid.empty())
            headers.push_back(current_header);
        return headers;
    }

    static std::string quote_imap(const std::string& value) {
        std::string result = "\"";
        for (char ch : value) {
            if (ch == '"' || ch == '\\') result += '\\';
            result += ch;
        }
        result += '"';
        return result;
    }

    // Read all server responses until we see our tag
    std::string read_until_tag(const std::string& expected_tag) {
        std::string accumulated;
        for (;;) {
            std::string line = connection_->recv_line();
            if (line.empty()) break;
            accumulated += line;
            // Check for IMAP literal: ends with {N}\r\n
            std::string trimmed = trim_string(line);
            if (!trimmed.empty() && trimmed.back() == '}') {
                size_t brace_open = trimmed.rfind('{');
                if (brace_open != std::string::npos) {
                    std::string size_str = trimmed.substr(brace_open + 1, trimmed.size() - brace_open - 2);
                    size_t literal_size = (size_t)std::stoul(size_str);
                    std::string literal_data = connection_->recv_exact(literal_size);
                    accumulated += literal_data;
                    continue;
                }
            }
            // Check if this is our tagged response
            if (line.find(expected_tag + " ") == 0) break;
        }
        return accumulated;
    }

    // Parse MIME body from RFC822 response
    std::string parse_mime_body(const std::string& raw_response) {
        last_attachments_.clear();
        raw_html_.clear();
        cid_images_.clear();
        // read_until_tag already fetched the literal via recv_exact and stored it
        // in accumulated right after the {N}\r\n framing line. Find that boundary.
        std::string full_message = raw_response;
        size_t search_pos = 0;
        while (search_pos < raw_response.size()) {
            size_t brace_open = raw_response.find('{', search_pos);
            if (brace_open == std::string::npos) break;
            size_t brace_close = raw_response.find('}', brace_open + 1);
            if (brace_close == std::string::npos) break;
            bool is_number = (brace_close > brace_open + 1);
            for (size_t pos = brace_open + 1; pos < brace_close && is_number; ++pos)
                if (!isdigit((unsigned char)raw_response[pos])) is_number = false;
            if (is_number) {
                size_t literal_size = (size_t)std::stoull(
                    raw_response.substr(brace_open + 1, brace_close - brace_open - 1));
                size_t newline_pos = raw_response.find('\n', brace_close);
                if (newline_pos != std::string::npos) {
                    size_t literal_start = newline_pos + 1;
                    size_t available = raw_response.size() > literal_start
                                       ? raw_response.size() - literal_start : 0;
                    full_message = raw_response.substr(literal_start,
                                                       std::min(literal_size, available));
                    debug_log("MIME: literal found size=" + std::to_string(literal_size) +
                              " first80=" + full_message.substr(0, 80));
                    break;
                }
            }
            search_pos = brace_close + 1;
        }
        // Replace cid: references in raw_html_ with data URIs from cid_images_
        // so inline images (e.g. email signatures) render correctly in the browser.
        auto replace_cid_references = [&]() {
            if (raw_html_.empty() || cid_images_.empty()) return;
            for (const auto& cid_entry : cid_images_) {
                const std::string& cid_key = cid_entry.first;
                const std::string& data_uri = cid_entry.second;
                std::string search_token = "cid:" + cid_key;
                size_t replace_pos = 0;
                while ((replace_pos = raw_html_.find(search_token, replace_pos)) != std::string::npos) {
                    raw_html_.replace(replace_pos, search_token.size(), data_uri);
                    replace_pos += data_uri.size();
                }
            }
        };

        debug_log("MIME: full_message first300=[" +
                  full_message.substr(0, std::min(full_message.size(), (size_t)300)) + "]");
        std::string result = extract_text_from_mime(full_message);
        debug_log("MIME: result length=" + std::to_string(result.size()) +
                  " first200=[" + result.substr(0, std::min(result.size(), (size_t)200)) + "]");

        // Post-process 1: if result looks like raw MIME (starts with "--"), try multipart
        // extraction directly from the result. Catches cases where extract_text_from_mime
        // returned body_section verbatim when it contains the multipart structure.
        if (!result.empty()) {
            size_t nws = result.find_first_not_of(" \r\n\t");
            if (nws != std::string::npos && nws + 1 < result.size() &&
                result[nws] == '-' && result[nws + 1] == '-') {
                size_t line_end_pos = result.find_first_of("\r\n", nws + 2);
                if (line_end_pos == std::string::npos) line_end_pos = result.size();
                std::string mime_boundary = result.substr(nws, line_end_pos - nws);
                debug_log("MIME postproc1: raw MIME in result, boundary=[" + mime_boundary.substr(0, 60) + "]");
                if (mime_boundary.size() > 4) {
                    std::string reparsed = extract_multipart(result, mime_boundary);
                    if (!reparsed.empty()) {
                        debug_log("MIME postproc1: success len=" + std::to_string(reparsed.size()));
                        result = reparsed;
                    }
                }
            }
        }

        // Post-process 2: if result still looks like raw MIME, scan full_message directly
        // for any "--boundary" line and attempt multipart extraction from the source.
        if (!result.empty()) {
            size_t nws = result.find_first_not_of(" \r\n\t");
            if (nws != std::string::npos && nws + 1 < result.size() &&
                result[nws] == '-' && result[nws + 1] == '-') {
                // result still starts with "--" after postproc1 — scan full_message
                size_t boundary_line = full_message.find("\n--");
                if (boundary_line == std::string::npos) boundary_line = full_message.find("\r--");
                if (boundary_line != std::string::npos) {
                    size_t bstart = boundary_line + 1;
                    size_t bend   = full_message.find_first_of("\r\n", bstart + 2);
                    if (bend == std::string::npos) bend = full_message.size();
                    std::string fm_boundary = full_message.substr(bstart, bend - bstart);
                    debug_log("MIME postproc2: boundary from full_message=[" + fm_boundary.substr(0, 60) + "]");
                    if (fm_boundary.size() > 4) {
                        std::string reparsed = extract_multipart(full_message, fm_boundary);
                        if (!reparsed.empty()) {
                            debug_log("MIME postproc2: success len=" + std::to_string(reparsed.size()));
                            result = reparsed;
                        }
                    }
                }
            }
        }

        // Post-process 3: strip raw HTML (e.g. unusual message structure).
        if (!result.empty()) {
            std::string leading = result.substr(0, std::min(result.size(), (size_t)200));
            std::string leading_lower = to_lower(leading);
            bool looks_like_html = (leading_lower.find("<html")    != std::string::npos ||
                                    leading_lower.find("<!doctype") != std::string::npos ||
                                    leading_lower.find("</head")   != std::string::npos);
            if (looks_like_html) {
                debug_log("MIME: fallback strip_html triggered");
                std::string maybe_qp = decode_quoted_printable(result);
                result = clean_text_body(strip_html(maybe_qp));
            }
        }
        replace_cid_references();
        return result;
    }

    std::string extract_text_from_mime(const std::string& mime_data) {
        // Find content-type
        size_t header_end = mime_data.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            header_end = mime_data.find("\n\n");
            if (header_end == std::string::npos) return mime_data;
            header_end += 2;
        } else {
            header_end += 4;
        }

        // Keep both original and lowercased headers:
        // original for extracting boundary/charset values (case-sensitive),
        // lowercased for field name lookups (case-insensitive).
        std::string header_original = mime_data.substr(0, header_end);
        std::string header_lower = to_lower(header_original);
        std::string body_section = mime_data.substr(header_end);
        debug_log("MIME etf: header_end=" + std::to_string(header_end) +
                  " body_len=" + std::to_string(body_section.size()));

        // Determine content-transfer-encoding
        // Find a header field name only at the start of a line (pos 0 or after '\n'),
        // so we never match field names embedded inside another header's value (e.g.
        // DKIM-Signature h= lists "content-type:" as a parameter, not a real header).
        auto find_header_start = [](const std::string& lower, const std::string& name) -> size_t {
            size_t search_pos = 0;
            while (search_pos < lower.size()) {
                size_t found = lower.find(name, search_pos);
                if (found == std::string::npos) return std::string::npos;
                if (found == 0 || lower[found - 1] == '\n') return found;
                search_pos = found + 1;
            }
            return std::string::npos;
        };

        std::string transfer_encoding;
        size_t cte_pos = find_header_start(header_lower, "content-transfer-encoding:");
        if (cte_pos != std::string::npos) {
            size_t cte_end = header_lower.find('\n', cte_pos);
            transfer_encoding = trim_string(to_lower(header_original.substr(cte_pos + 26, cte_end - cte_pos - 26)));
        }

        // Check content-type
        size_t ct_pos = find_header_start(header_lower, "content-type:");
        std::string content_type;
        if (ct_pos != std::string::npos) {
            size_t ct_end = header_lower.find('\n', ct_pos);
            content_type = trim_string(header_lower.substr(ct_pos + 13, ct_end - ct_pos - 13));
        }
        debug_log("MIME etf: ct=[" + content_type + "] te=[" + transfer_encoding + "]");

        // Extract charset from full header (may appear on a continuation line after content-type)
        std::string charset_name = "utf-8";
        size_t charset_pos = header_lower.find("charset=");
        if (charset_pos != std::string::npos) {
            size_t charset_value_start = charset_pos + 8;
            if (charset_value_start < header_original.size() && header_original[charset_value_start] == '"') {
                size_t charset_end = header_original.find('"', charset_value_start + 1);
                if (charset_end == std::string::npos) charset_end = header_original.size();
                charset_name = to_lower(trim_string(
                    header_original.substr(charset_value_start + 1, charset_end - charset_value_start - 1)));
            } else {
                size_t charset_end = header_original.find_first_of(" \r\n;\"", charset_value_start);
                if (charset_end == std::string::npos) charset_end = header_original.size();
                charset_name = to_lower(trim_string(
                    header_original.substr(charset_value_start, charset_end - charset_value_start)));
            }
        }

        if (content_type.find("multipart/") != std::string::npos) {
            // Find boundary= position in lowercase header, then extract the actual
            // value from the original header to preserve exact case of the boundary string.
            size_t boundary_pos = header_lower.find("boundary=");
            if (boundary_pos != std::string::npos) {
                size_t boundary_start = boundary_pos + 9;
                std::string boundary_value;
                if (boundary_start < header_original.size() && header_original[boundary_start] == '"') {
                    size_t boundary_end = header_original.find('"', boundary_start + 1);
                    if (boundary_end == std::string::npos) boundary_end = header_original.size();
                    boundary_value = header_original.substr(boundary_start + 1, boundary_end - boundary_start - 1);
                } else {
                    size_t boundary_end = header_original.find_first_of(" \r\n;", boundary_start);
                    if (boundary_end == std::string::npos) boundary_end = header_original.size();
                    boundary_value = trim_string(header_original.substr(boundary_start, boundary_end - boundary_start));
                }
                if (!boundary_value.empty())
                    return extract_multipart(mime_data.substr(header_end), "--" + boundary_value);
            }
            // boundary= not found or empty in headers — fall through to body auto-detect
        }

        // Fallback: if body starts with a "--" boundary line, auto-detect the boundary
        // and attempt multipart parsing. Handles Python email.mime messages where the
        // boundary= parameter is missing, folded, or otherwise not parseable from headers.
        {
            size_t body_nws = body_section.find_first_not_of("\r\n \t");
            if (body_nws != std::string::npos && body_nws + 1 < body_section.size() &&
                body_section[body_nws] == '-' && body_section[body_nws + 1] == '-') {
                size_t line_end = body_section.find_first_of("\r\n", body_nws + 2);
                if (line_end == std::string::npos) line_end = body_section.size();
                std::string auto_boundary = body_section.substr(body_nws, line_end - body_nws);
                debug_log("MIME: auto-boundary [" + auto_boundary.substr(0, 60) + "]");
                if (auto_boundary.size() > 4) {
                    std::string attempt = extract_multipart(body_section, auto_boundary);
                    if (!attempt.empty()) return attempt;
                }
            }
        }

        // Decode body
        std::string decoded_body;
        if (transfer_encoding == "base64") {
            std::string base64_data;
            for (char ch : body_section) if (ch != '\r' && ch != '\n') base64_data += ch;
            decoded_body = decode_base64(base64_data);
        } else if (transfer_encoding == "quoted-printable") {
            decoded_body = decode_quoted_printable(body_section);
        } else {
            decoded_body = body_section;
        }

        // Discard binary content (attachments that leaked through MIME parsing)
        if (looks_like_binary(decoded_body)) return {};

        // Convert from email charset to UTF-8 so the UI renders correctly
        decoded_body = convert_charset_to_utf8(decoded_body, charset_name);

        // Determine if content is HTML: either by explicit content-type,
        // or by sniffing the body when content-type is missing/unknown.
        bool is_html = content_type.find("text/html") != std::string::npos;
        if (!is_html) {
            std::string body_start = to_lower(decoded_body.substr(0, std::min(decoded_body.size(), (size_t)256)));
            is_html = body_start.find("<html")     != std::string::npos
                   || body_start.find("<!doctype") != std::string::npos;
        }
        if (is_html) {
            if (raw_html_.empty()) raw_html_ = decoded_body;
            return clean_text_body(strip_html(decoded_body));
        }
        return clean_text_body(decoded_body);
    }

    // Extract a named parameter value from a MIME header (e.g. filename="foo.pdf" → "foo.pdf").
    // header_lower and header_original must be the same text, just differing in case.
    static std::string extract_mime_param(const std::string& header_lower,
                                          const std::string& header_original,
                                          const std::string& param_name) {
        std::string search = param_name + "=";
        size_t param_pos = header_lower.find(search);
        if (param_pos == std::string::npos) return {};
        size_t value_start = param_pos + search.size();
        if (value_start >= header_original.size()) return {};
        if (header_original[value_start] == '"') {
            size_t value_end = header_original.find('"', value_start + 1);
            if (value_end == std::string::npos) value_end = header_original.size();
            return header_original.substr(value_start + 1, value_end - value_start - 1);
        }
        size_t value_end = header_original.find_first_of(" \r\n;\"", value_start);
        if (value_end == std::string::npos) value_end = header_original.size();
        return trim_string(header_original.substr(value_start, value_end - value_start));
    }

    std::string extract_multipart(const std::string& body, const std::string& boundary) {
        std::string plain_part;
        std::string html_part;

        size_t pos = 0;
        while (pos < body.size()) {
            size_t part_start = body.find(boundary, pos);
            if (part_start == std::string::npos) break;
            part_start += boundary.size();
            if (part_start >= body.size()) break;
            if (body[part_start] == '-') break; // closing boundary --boundary--

            size_t content_start = body.find('\n', part_start);
            if (content_start == std::string::npos) break;
            content_start += 1;

            size_t part_end = body.find(boundary, content_start);
            if (part_end == std::string::npos) part_end = body.size();
            else {
                // Back up over the CRLF that precedes the boundary marker (only \r and \n,
                // not '-' — dashes are legal content characters and would strip content).
                while (part_end > content_start &&
                       (body[part_end - 1] == '\r' || body[part_end - 1] == '\n'))
                    --part_end;
            }

            std::string part_data = body.substr(content_start, part_end - content_start);
            // Find the actual header/body separator (\r\n\r\n or \n\n)
            size_t header_body_sep = part_data.find("\r\n\r\n");
            size_t sep_len = 4;
            if (header_body_sep == std::string::npos) {
                header_body_sep = part_data.find("\n\n");
                sep_len = 2;
            }
            size_t part_header_len = (header_body_sep != std::string::npos)
                                     ? header_body_sep + sep_len
                                     : std::min(part_data.size(), (size_t)2048);
            std::string part_original_header = part_data.substr(0, part_header_len);
            std::string part_lower_header = to_lower(part_original_header);

            if (part_lower_header.find("text/plain") != std::string::npos) {
                plain_part = extract_text_from_mime(part_data);
            } else if (part_lower_header.find("text/html") != std::string::npos) {
                // Always extract HTML — stores raw HTML in raw_html_ for the browser view
                // even when a plain-text part is already available.
                html_part = extract_text_from_mime(part_data);
            } else if (part_lower_header.find("multipart/") != std::string::npos) {
                // Nested multipart (e.g. multipart/alternative inside multipart/mixed)
                std::string nested_text = extract_text_from_mime(part_data);
                if (!nested_text.empty() && plain_part.empty()) {
                    plain_part = nested_text;
                }
            } else {
                // Non-text part — collect as attachment
                collect_attachment_part(part_data, part_lower_header, part_original_header);
            }

            pos = part_end;
        }

        if (!plain_part.empty()) return plain_part;
        return html_part;
    }

    void collect_attachment_part(const std::string& part_data,
                                 const std::string& part_lower_header,
                                 const std::string& part_original_header) {
        // Find part header / body boundary
        size_t part_header_end = part_data.find("\r\n\r\n");
        if (part_header_end == std::string::npos) {
            part_header_end = part_data.find("\n\n");
            if (part_header_end == std::string::npos) return;
            part_header_end += 2;
        } else {
            part_header_end += 4;
        }
        std::string part_body = part_data.substr(part_header_end);

        // Determine transfer encoding
        std::string transfer_encoding;
        size_t cte_pos = part_lower_header.find("content-transfer-encoding:");
        if (cte_pos != std::string::npos) {
            size_t cte_end = part_lower_header.find('\n', cte_pos);
            transfer_encoding = trim_string(part_lower_header.substr(cte_pos + 26,
                                            cte_end == std::string::npos ? std::string::npos
                                                                         : cte_end - cte_pos - 26));
        }

        // Extract Content-ID for inline images (cid: references in HTML)
        std::string content_id;
        size_t cid_pos = part_lower_header.find("content-id:");
        if (cid_pos != std::string::npos) {
            size_t cid_end = part_lower_header.find('\n', cid_pos);
            content_id = trim_string(part_original_header.substr(cid_pos + 11,
                         cid_end == std::string::npos ? std::string::npos : cid_end - cid_pos - 11));
            // Strip angle brackets: <signature_123> → signature_123
            if (!content_id.empty() && content_id.front() == '<') content_id = content_id.substr(1);
            if (!content_id.empty() && content_id.back()  == '>') content_id.pop_back();
            content_id = trim_string(content_id);
        }

        // Decode body — keep raw base64 string for inline images (avoids decode+re-encode)
        std::string raw_base64;
        std::string raw_bytes;
        if (transfer_encoding.find("base64") != std::string::npos) {
            for (char ch : part_body) if (ch != '\r' && ch != '\n') raw_base64 += ch;
            if (!content_id.empty()) {
                // For inline images we use the raw base64 directly; skip full decode
            } else {
                raw_bytes = decode_base64(raw_base64);
            }
        } else {
            raw_bytes = part_body;
        }

        // If this part has a Content-ID and is an image, store as data URI for HTML rendering
        if (!content_id.empty()) {
            size_t ct_cid_pos = part_lower_header.find("content-type:");
            std::string image_mime_type = "image/png";
            if (ct_cid_pos != std::string::npos) {
                size_t ct_cid_end = part_lower_header.find_first_of(";\r\n", ct_cid_pos + 13);
                image_mime_type = trim_string(part_lower_header.substr(ct_cid_pos + 13,
                    ct_cid_end == std::string::npos ? std::string::npos : ct_cid_end - ct_cid_pos - 13));
            }
            std::string data_uri = "data:" + image_mime_type + ";base64,";
            if (!raw_base64.empty()) {
                data_uri += raw_base64;
            } else {
                data_uri += encode_base64(raw_bytes);
            }
            cid_images_[content_id] = std::move(data_uri);
            return;  // inline images are not listed as downloadable attachments
        }

        if (raw_bytes.empty()) return;

        // Extract filename: prefer Content-Disposition filename=, fallback to name=
        std::string filename = extract_mime_param(part_lower_header, part_original_header, "filename");
        if (filename.empty()) filename = extract_mime_param(part_lower_header, part_original_header, "name");

        // Decode RFC2047 encoded-words in filename (=?charset?B/Q?...?=)
        if (!filename.empty()) filename = decode_rfc2047(filename);

        // Try RFC2231 filename* (e.g. filename*=UTF-8''hello%20world.pdf)
        if (filename.empty()) {
            std::string rfc2231_value = extract_mime_param(part_lower_header, part_original_header, "filename*");
            if (rfc2231_value.empty())
                rfc2231_value = extract_mime_param(part_lower_header, part_original_header, "name*");
            if (!rfc2231_value.empty()) {
                size_t first_quote = rfc2231_value.find('\'');
                if (first_quote != std::string::npos) {
                    size_t second_quote = rfc2231_value.find('\'', first_quote + 1);
                    if (second_quote != std::string::npos) {
                        std::string charset = rfc2231_value.substr(0, first_quote);
                        std::string encoded_name = rfc2231_value.substr(second_quote + 1);
                        std::string decoded_name;
                        for (size_t idx = 0; idx < encoded_name.size(); ) {
                            if (encoded_name[idx] == '%' && idx + 2 < encoded_name.size()) {
                                std::string hex_str = encoded_name.substr(idx + 1, 2);
                                char byte_val = (char)strtol(hex_str.c_str(), nullptr, 16);
                                decoded_name += byte_val;
                                idx += 3;
                            } else {
                                decoded_name += encoded_name[idx++];
                            }
                        }
                        if (!decoded_name.empty())
                            filename = convert_charset_to_utf8(decoded_name, to_lower(charset));
                    }
                } else {
                    filename = rfc2231_value;
                }
            }
        }

        if (filename.empty()) {
            // Generate a name from content-type
            size_t ct_pos = part_lower_header.find("content-type:");
            std::string mime_type;
            if (ct_pos != std::string::npos) {
                size_t ct_end = part_lower_header.find_first_of(";\r\n", ct_pos + 13);
                mime_type = trim_string(part_lower_header.substr(ct_pos + 13,
                            ct_end == std::string::npos ? std::string::npos : ct_end - ct_pos - 13));
            }
            // Map common MIME types to extensions
            std::string extension = ".bin";
            if (mime_type.find("pdf")   != std::string::npos) extension = ".pdf";
            else if (mime_type.find("jpeg") != std::string::npos ||
                     mime_type.find("jpg")  != std::string::npos) extension = ".jpg";
            else if (mime_type.find("png")  != std::string::npos) extension = ".png";
            else if (mime_type.find("gif")  != std::string::npos) extension = ".gif";
            else if (mime_type.find("zip")  != std::string::npos) extension = ".zip";
            else if (mime_type.find("msword")       != std::string::npos) extension = ".doc";
            else if (mime_type.find("spreadsheet")  != std::string::npos) extension = ".xls";
            else if (mime_type.find("presentation") != std::string::npos) extension = ".ppt";
            filename = "attachment" + extension;
        }

        // Extract content-type
        std::string content_type;
        size_t ct_pos2 = part_lower_header.find("content-type:");
        if (ct_pos2 != std::string::npos) {
            size_t ct_end2 = part_lower_header.find_first_of(";\r\n", ct_pos2 + 13);
            content_type = trim_string(part_lower_header.substr(ct_pos2 + 13,
                           ct_end2 == std::string::npos ? std::string::npos : ct_end2 - ct_pos2 - 13));
        }

        AttachmentInfo att;
        att.filename     = filename;
        att.content_type = content_type;
        att.raw_bytes    = std::move(raw_bytes);
        last_attachments_.push_back(std::move(att));
    }
};

// ─── SMTP Client ─────────────────────────────────────────────────────────────
class SmtpClient {
public:
    SmtpClient() {}

    bool send_message(const std::string& email_address, const std::string& app_password,
                      const ComposeFields& compose_fields) {
        TlsConnection smtp_conn;
        const char* smtp_host = g_account.smtp_host.empty()
            ? SMTP_HOST : g_account.smtp_host.c_str();
        int smtp_port = g_account.smtp_port > 0
            ? g_account.smtp_port : SMTP_PORT;
        // Plain connect, then STARTTLS
        bool connected = smtp_conn.connect_starttls(smtp_host, smtp_port,
            [&](TlsConnection* conn) -> bool {
                // Read greeting
                std::string greeting = conn->recv_plain_line();
                if (greeting.find("220") == std::string::npos) return false;
                // EHLO
                conn->send_plain("EHLO mail.me.com\r\n");
                std::string ehlo_response;
                for (;;) {
                    std::string ehlo_line = conn->recv_plain_line();
                    ehlo_response += ehlo_line;
                    if (ehlo_line.size() >= 4 && ehlo_line[3] == ' ') break;
                    if (ehlo_line.empty()) return false;
                }
                // STARTTLS
                conn->send_plain("STARTTLS\r\n");
                std::string starttls_response = conn->recv_plain_line();
                return starttls_response.find("220") != std::string::npos;
            });

        if (!connected) { last_error_ = "STARTTLS connection failed"; return false; }

        // Now TLS is active — EHLO again
        smtp_conn.send_data("EHLO mail.me.com\r\n");
        std::string ehlo2_response;
        for (;;) {
            std::string ehlo_line = smtp_conn.recv_line();
            ehlo2_response += ehlo_line;
            if (ehlo_line.size() >= 4 && ehlo_line[3] == ' ') break;
            if (ehlo_line.empty()) break;
        }

        // AUTH LOGIN
        smtp_conn.send_data("AUTH LOGIN\r\n");
        smtp_conn.recv_line(); // 334 VXNlcm5hbWU6
        smtp_conn.send_data(encode_base64(email_address) + "\r\n");
        smtp_conn.recv_line(); // 334 UGFzc3dvcmQ6
        smtp_conn.send_data(encode_base64(app_password) + "\r\n");
        std::string auth_response = smtp_conn.recv_line();
        if (auth_response.find("235") == std::string::npos) {
            last_error_ = "AUTH LOGIN failed";
            return false;
        }

        // MAIL FROM
        smtp_conn.send_data("MAIL FROM:<" + email_address + ">\r\n");
        smtp_conn.recv_line();

        // RCPT TO — parse To and Cc
        std::string to_str = wide_to_utf8(compose_fields.to_addresses);
        std::string cc_str = wide_to_utf8(compose_fields.cc_addresses);
        auto send_rcpt = [&](const std::string& addresses) {
            if (addresses.empty()) return;
            std::istringstream stream(addresses);
            std::string address;
            while (std::getline(stream, address, ',')) {
                address = trim_string(address);
                if (!address.empty()) {
                    smtp_conn.send_data("RCPT TO:<" + address + ">\r\n");
                    smtp_conn.recv_line();
                }
            }
        };
        send_rcpt(to_str);
        send_rcpt(cc_str);

        // DATA
        smtp_conn.send_data("DATA\r\n");
        smtp_conn.recv_line(); // 354

        // Build message
        std::string subject_utf8  = wide_to_utf8(compose_fields.subject_text);
        std::string body_utf8     = wide_to_utf8(compose_fields.body_text);
        std::string from_utf8     = wide_to_utf8(compose_fields.from_address);

        std::string message;
        message += "From: " + from_utf8 + "\r\n";
        message += "To: " + to_str + "\r\n";
        if (!cc_str.empty()) message += "Cc: " + cc_str + "\r\n";
        message += "Subject: =?UTF-8?B?" + encode_base64(subject_utf8) + "?=\r\n";
        message += "MIME-Version: 1.0\r\n";
        message += "Content-Type: text/plain; charset=UTF-8\r\n";
        message += "Content-Transfer-Encoding: base64\r\n";
        message += "\r\n";
        // Encode body in base64, 76 chars per line
        std::string encoded_body = encode_base64(body_utf8);
        for (size_t line_pos = 0; line_pos < encoded_body.size(); line_pos += 76) {
            message += encoded_body.substr(line_pos, 76) + "\r\n";
        }
        message += ".\r\n";

        smtp_conn.send_data(message);
        std::string data_response = smtp_conn.recv_line();
        if (data_response.find("250") == std::string::npos) {
            last_error_ = "DATA rejected: " + data_response;
            return false;
        }

        smtp_conn.send_data("QUIT\r\n");
        smtp_conn.recv_line();
        return true;
    }

    std::string last_error_;
};

// ─── Account settings window (full impl, after ImapClient/SmtpClient) ────────
static LRESULT CALLBACK settings_wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hinst = (HINSTANCE)GetWindowLongPtrW(wnd, GWLP_HINSTANCE);
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT bold = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SetWindowLongPtrW(wnd, GWLP_USERDATA, (LPARAM)bold);

        HWND title_lbl = CreateWindowW(L"STATIC", L"Account Settings",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 14, 380, 22, wnd, nullptr, hinst, nullptr);
        SendMessageW(title_lbl, WM_SETFONT, (WPARAM)bold, TRUE);

        CreateWindowW(L"BUTTON", L"iCloud",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            10, 46, 85, 70, wnd, (HMENU)IDC_PROV_ICLOUD, hinst, nullptr);
        CreateWindowW(L"BUTTON", L"Gmail",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            100, 46, 85, 70, wnd, (HMENU)IDC_PROV_GMAIL, hinst, nullptr);
        CreateWindowW(L"BUTTON", L"Outlook",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            190, 46, 85, 70, wnd, (HMENU)IDC_PROV_OUTLOOK, hinst, nullptr);
        CreateWindowW(L"BUTTON", L"Exchange",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            280, 46, 85, 70, wnd, (HMENU)IDC_PROV_EXCHANGE, hinst, nullptr);

        auto make_label = [&](const wchar_t* text, int label_x, int label_y) {
            HWND label_wnd = CreateWindowW(L"STATIC", text,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                label_x, label_y, 160, 16, wnd, nullptr, hinst, nullptr);
            SendMessageW(label_wnd, WM_SETFONT, (WPARAM)font, TRUE);
        };
        auto make_edit = [&](int edit_id, int edit_x, int edit_y, int edit_w,
                              bool password_mode, const std::wstring& initial_text) -> HWND {
            DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
            if (password_mode) style |= ES_PASSWORD;
            HWND edit_wnd = CreateWindowW(L"EDIT", L"",
                style, edit_x, edit_y, edit_w, 22, wnd, (HMENU)(LONG_PTR)edit_id,
                hinst, nullptr);
            SendMessageW(edit_wnd, WM_SETFONT, (WPARAM)font, TRUE);
            if (!initial_text.empty()) SetWindowTextW(edit_wnd, initial_text.c_str());
            return edit_wnd;
        };

        AccountConfig& cfg = g_settings_state.config;
        make_label(L"Email / Apple ID:", 16, 130);
        make_edit(IDC_SETTINGS_EMAIL, 16, 148, 360, false, utf8_to_wide(cfg.email));
        make_label(L"App Password:", 16, 176);
        make_edit(IDC_SETTINGS_PASS,  16, 194, 360, true,  utf8_to_wide(cfg.password));
        make_label(L"IMAP Host:", 16, 224);
        make_edit(IDC_SETTINGS_HOST,  16, 242, 280, false, utf8_to_wide(cfg.imap_host));
        make_label(L"Port:", 308, 224);
        make_edit(IDC_SETTINGS_PORT, 308, 242, 68, false, std::to_wstring(cfg.imap_port));
        make_label(L"SMTP Host:", 16, 272);
        make_edit(IDC_SETTINGS_SMTP,  16, 290, 280, false, utf8_to_wide(cfg.smtp_host));
        make_label(L"Port:", 308, 272);
        make_edit(IDC_SETTINGS_SPORT, 308, 290, 68, false, std::to_wstring(cfg.smtp_port));

        auto make_btn = [&](const wchar_t* text, int btn_id, int btn_x, int btn_y, int btn_w) {
            HWND btn_wnd = CreateWindowW(L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                btn_x, btn_y, btn_w, 28, wnd, (HMENU)(LONG_PTR)btn_id, hinst, nullptr);
            SendMessageW(btn_wnd, WM_SETFONT, (WPARAM)font, TRUE);
            return btn_wnd;
        };
        make_btn(L"Test Connection", IDC_SETTINGS_TEST,   16, 334, 130);
        make_btn(L"Save",            IDC_SETTINGS_SAVE,  220, 334, 70);
        make_btn(L"Cancel",          IDC_SETTINGS_CANCEL, 300, 334, 76);

        CheckDlgButton(wnd, IDC_PROV_ICLOUD,
            cfg.provider == "icloud"    ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(wnd, IDC_PROV_GMAIL,
            cfg.provider == "gmail"     ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(wnd, IDC_PROV_OUTLOOK,
            cfg.provider == "outlook"   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(wnd, IDC_PROV_EXCHANGE,
            cfg.provider == "exchange"  ? BST_CHECKED : BST_UNCHECKED);
        InvalidateRect(wnd, nullptr, TRUE);
        return 0;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
        RECT card_rc    = dis->rcItem;
        bool is_pressed  = (dis->itemState & ODS_SELECTED) != 0;
        bool is_icloud   = (dis->CtlID == IDC_PROV_ICLOUD);
        bool is_gmail    = (dis->CtlID == IDC_PROV_GMAIL);
        bool is_outlook  = (dis->CtlID == IDC_PROV_OUTLOOK);
        bool is_exchange = (dis->CtlID == IDC_PROV_EXCHANGE);
        bool is_checked  = (IsDlgButtonChecked(wnd, dis->CtlID) == BST_CHECKED);

        COLORREF bg_color = is_checked ? RGB(230, 245, 255) : RGB(248, 248, 248);
        if (is_pressed) bg_color = RGB(200, 230, 255);
        HBRUSH card_br = CreateSolidBrush(bg_color);
        FillRect(dis->hDC, &card_rc, card_br);
        DeleteObject(card_br);

        HPEN border_pen = CreatePen(PS_SOLID, 2,
            is_checked ? RGB(0, 120, 212) : RGB(200, 200, 200));
        HPEN old_pen  = (HPEN)SelectObject(dis->hDC, border_pen);
        HBRUSH old_br = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
        Rectangle(dis->hDC, card_rc.left, card_rc.top, card_rc.right, card_rc.bottom);
        SelectObject(dis->hDC, old_pen);
        SelectObject(dis->hDC, old_br);
        DeleteObject(border_pen);

        int icon_size = 40;
        int icon_x = card_rc.left + (card_rc.right - card_rc.left) / 2 - icon_size / 2;
        int icon_y = card_rc.top + 8;
        if (is_icloud)
            draw_icloud_icon(dis->hDC, icon_x, icon_y, icon_size);
        else if (is_gmail)
            draw_gmail_icon(dis->hDC, icon_x, icon_y, icon_size);
        else if (is_outlook)
            draw_outlook_icon(dis->hDC, icon_x, icon_y, icon_size);
        else if (is_exchange)
            draw_exchange_icon(dis->hDC, icon_x, icon_y, icon_size);

        const wchar_t* label_text = is_icloud   ? L"iCloud"
                                  : is_gmail    ? L"Gmail"
                                  : is_outlook  ? L"Outlook"
                                  : L"Exchange";
        SetTextColor(dis->hDC, RGB(30, 30, 30));
        SetBkMode(dis->hDC, TRANSPARENT);
        RECT text_rc = { card_rc.left, card_rc.top + icon_size + 12,
                         card_rc.right, card_rc.bottom };
        DrawTextW(dis->hDC, label_text, -1, &text_rc, DT_CENTER | DT_SINGLELINE);
        return TRUE;
    }

    case WM_COMMAND: {
        int cmd_id = LOWORD(wp);
        if (cmd_id == IDC_PROV_ICLOUD || cmd_id == IDC_PROV_GMAIL
         || cmd_id == IDC_PROV_OUTLOOK || cmd_id == IDC_PROV_EXCHANGE) {
            CheckDlgButton(wnd, IDC_PROV_ICLOUD,
                cmd_id == IDC_PROV_ICLOUD   ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(wnd, IDC_PROV_GMAIL,
                cmd_id == IDC_PROV_GMAIL    ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(wnd, IDC_PROV_OUTLOOK,
                cmd_id == IDC_PROV_OUTLOOK  ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(wnd, IDC_PROV_EXCHANGE,
                cmd_id == IDC_PROV_EXCHANGE ? BST_CHECKED : BST_UNCHECKED);
            const AccountConfig& preset =
                (cmd_id == IDC_PROV_ICLOUD)   ? PRESET_ICLOUD   :
                (cmd_id == IDC_PROV_GMAIL)    ? PRESET_GMAIL    :
                (cmd_id == IDC_PROV_OUTLOOK)  ? PRESET_OUTLOOK  :
                                                PRESET_EXCHANGE;
            // Only fill server fields — email/password are per-account and must
            // NOT be carried over when the user switches to a different provider.
            SetDlgItemTextW(wnd, IDC_SETTINGS_HOST,
                utf8_to_wide(preset.imap_host).c_str());
            SetDlgItemTextW(wnd, IDC_SETTINGS_PORT,
                std::to_wstring(preset.imap_port).c_str());
            SetDlgItemTextW(wnd, IDC_SETTINGS_SMTP,
                utf8_to_wide(preset.smtp_host).c_str());
            SetDlgItemTextW(wnd, IDC_SETTINGS_SPORT,
                std::to_wstring(preset.smtp_port).c_str());
            // Clear credentials — new provider = new independent account
            SetDlgItemTextW(wnd, IDC_SETTINGS_EMAIL, L"");
            SetDlgItemTextW(wnd, IDC_SETTINGS_PASS,  L"");
            InvalidateRect(wnd, nullptr, TRUE);
        } else if (cmd_id == IDC_SETTINGS_SAVE) {
            AccountConfig& cfg = g_settings_state.config;
            wchar_t text_buf[512];
            GetDlgItemTextW(wnd, IDC_SETTINGS_EMAIL, text_buf, 512);
            cfg.email     = wide_to_utf8(text_buf);
            GetDlgItemTextW(wnd, IDC_SETTINGS_PASS,  text_buf, 512);
            cfg.password  = wide_to_utf8(text_buf);
            GetDlgItemTextW(wnd, IDC_SETTINGS_HOST,  text_buf, 512);
            cfg.imap_host = wide_to_utf8(text_buf);
            GetDlgItemTextW(wnd, IDC_SETTINGS_PORT,  text_buf, 512);
            cfg.imap_port = _wtoi(text_buf);
            GetDlgItemTextW(wnd, IDC_SETTINGS_SMTP,  text_buf, 512);
            cfg.smtp_host = wide_to_utf8(text_buf);
            GetDlgItemTextW(wnd, IDC_SETTINGS_SPORT, text_buf, 512);
            cfg.smtp_port = _wtoi(text_buf);
            if (IsDlgButtonChecked(wnd, IDC_PROV_ICLOUD) == BST_CHECKED) {
                cfg.provider = "icloud";    cfg.display_name = "iCloud";
            } else if (IsDlgButtonChecked(wnd, IDC_PROV_GMAIL) == BST_CHECKED) {
                cfg.provider = "gmail";     cfg.display_name = "Gmail";
            } else if (IsDlgButtonChecked(wnd, IDC_PROV_OUTLOOK) == BST_CHECKED) {
                cfg.provider = "outlook";   cfg.display_name = "Outlook";
            } else if (IsDlgButtonChecked(wnd, IDC_PROV_EXCHANGE) == BST_CHECKED) {
                cfg.provider = "exchange";  cfg.display_name = "Exchange";
            } else {
                cfg.provider = "custom";    cfg.display_name = "Custom";
            }
            g_settings_state.ok = true;
            // Save by index so accounts with the same email address don't
            // overwrite each other and each account stays truly independent.
            int idx = g_settings_state.edit_index;
            if (idx >= 0 && idx < (int)g_accounts.size()) {
                g_accounts[idx] = cfg;            // update existing
            } else {
                g_accounts.push_back(cfg);        // add new
                g_active_account_index = (int)g_accounts.size() - 1;
            }
            save_all_accounts(g_accounts);
            save_account_config(cfg);  // also update single-account compat file
            DestroyWindow(wnd);
        } else if (cmd_id == IDC_SETTINGS_CANCEL) {
            DestroyWindow(wnd);
        } else if (cmd_id == IDC_SETTINGS_TEST) {
            wchar_t email_buf[256], pass_buf[256], host_buf[256], port_buf[16];
            GetDlgItemTextW(wnd, IDC_SETTINGS_EMAIL, email_buf, 256);
            GetDlgItemTextW(wnd, IDC_SETTINGS_PASS,  pass_buf,  256);
            GetDlgItemTextW(wnd, IDC_SETTINGS_HOST,  host_buf,  256);
            GetDlgItemTextW(wnd, IDC_SETTINGS_PORT,  port_buf,   16);
            std::string test_email = wide_to_utf8(email_buf);
            std::string test_pass  = wide_to_utf8(pass_buf);
            std::string test_host  = wide_to_utf8(host_buf);
            int test_port = _wtoi(port_buf);
            HWND test_btn = GetDlgItem(wnd, IDC_SETTINGS_TEST);
            SetWindowTextW(test_btn, L"Testing...");
            EnableWindow(test_btn, FALSE);
            HWND wnd_capture = wnd;
            std::thread([test_email, test_pass, test_host, test_port,
                         test_btn, wnd_capture]() {
                // Temporarily override g_account for the test
                AccountConfig temp_cfg = g_account;
                if (!test_host.empty())  temp_cfg.imap_host = test_host;
                if (test_port > 0)       temp_cfg.imap_port = test_port;
                AccountConfig saved = g_account;
                g_account = temp_cfg;
                auto test_client = std::make_unique<ImapClient>();
                bool connected = test_client->connect(test_email, test_pass);
                g_account = saved;  // restore
                PostMessageW(wnd_capture, WM_USER + 1, connected ? 1 : 0, (LPARAM)test_btn);
            }).detach();
        }
        return 0;
    }

    case WM_USER + 1: {
        HWND test_btn  = (HWND)lp;
        bool connected = (wp != 0);
        SetWindowTextW(test_btn, connected ? L"Connected OK" : L"Failed");
        EnableWindow(test_btn, TRUE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC paint_dc = BeginPaint(wnd, &ps);
        RECT client_rc;
        GetClientRect(wnd, &client_rc);
        HBRUSH header_br = CreateSolidBrush(RGB(0, 90, 160));
        RECT header_rc   = { 0, 0, client_rc.right, 40 };
        FillRect(paint_dc, &header_rc, header_br);
        DeleteObject(header_br);
        EndPaint(wnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        if (HFONT bold_font = (HFONT)GetWindowLongPtrW(wnd, GWLP_USERDATA))
            DeleteObject(bold_font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

// edit_index: -1 = add new account, >=0 = edit g_accounts[edit_index]
static bool show_account_settings(HWND parent, int edit_index) {
    g_settings_state.ok         = false;
    g_settings_state.edit_index = edit_index;
    if (edit_index >= 0 && edit_index < (int)g_accounts.size()) {
        // Load the specific account to edit — never mix credentials between accounts
        g_settings_state.config = g_accounts[edit_index];
    } else {
        // Adding a new account — start completely fresh
        g_settings_state.config = PRESET_ICLOUD;
        g_settings_state.config.email    = {};
        g_settings_state.config.password = {};
    }

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = settings_wnd_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FastMailSettings";
    RegisterClassExW(&wc);  // ignore if already registered

    int screen_width  = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);
    int win_width = 400, win_height = 385;
    HWND settings_wnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"FastMailSettings",
        L"FastMail — Accounts",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        (screen_width  - win_width)  / 2,
        (screen_height - win_height) / 2,
        win_width, win_height,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (parent) EnableWindow(parent, FALSE);
    ShowWindow(settings_wnd, SW_SHOW);
    UpdateWindow(settings_wnd);

    MSG loop_msg;
    while (GetMessageW(&loop_msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(settings_wnd, &loop_msg)) {
            TranslateMessage(&loop_msg);
            DispatchMessageW(&loop_msg);
        }
        if (!IsWindow(settings_wnd)) break;
    }

    if (parent) { EnableWindow(parent, TRUE); SetForegroundWindow(parent); }
    return g_settings_state.ok;
}

// ─── Application state ────────────────────────────────────────────────────────
struct AppState {
    std::wstring email_address;
    std::wstring app_password;
    std::unique_ptr<ImapClient> imap_client;          // user interactions
    std::unique_ptr<ImapClient> prefetch_client;      // background prefetch only
    bool prefetch_connected = false;
    std::vector<MailHeader> current_headers;
    std::string current_uid;
    std::string current_body;
    std::string current_body_html;
    std::string current_from;
    std::vector<AttachmentInfo> current_attachments;
    std::string current_subject;
    std::string current_folder;
    std::mutex imap_mutex;
    bool imap_connected = false;
};

static AppState g_app;

// Full IMAP folder paths indexed by tree-item lParam
static std::vector<std::string> g_folder_paths;
// Incremented each time the user switches folders; lets load threads self-cancel
static std::atomic<int> g_load_generation{0};

// ─── Config file I/O ─────────────────────────────────────────────────────────
static std::wstring get_config_path() {
    wchar_t appdata_path[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata_path);
    std::wstring config_dir = std::wstring(appdata_path) + L"\\FastMail";
    CreateDirectoryW(config_dir.c_str(), nullptr);
    return config_dir + L"\\config.txt";
}

static void save_config(const std::wstring& email_address, const std::wstring& app_password) {
    std::wstring path = get_config_path();
    // Ensure the directory exists before writing (get_config_path already does this,
    // but we call CreateDirectoryW here too in case the directory was removed at runtime)
    std::wstring dir_path = path.substr(0, path.rfind(L'\\'));
    CreateDirectoryW(dir_path.c_str(), nullptr); // OK if already exists
    HANDLE file_handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) return;
    std::string content = wide_to_utf8(email_address) + "\n" + wide_to_utf8(app_password) + "\n";
    DWORD written;
    WriteFile(file_handle, content.data(), (DWORD)content.size(), &written, nullptr);
    CloseHandle(file_handle);
}

static bool load_config(std::wstring& email_address, std::wstring& app_password) {
    std::wstring path = get_config_path();
    HANDLE file_handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) return false;
    DWORD file_size = GetFileSize(file_handle, nullptr);
    if (file_size == 0 || file_size > 65536) { CloseHandle(file_handle); return false; }
    std::string content(file_size, '\0');
    DWORD read_count;
    ReadFile(file_handle, content.data(), file_size, &read_count, nullptr);
    CloseHandle(file_handle);
    size_t newline_pos = content.find('\n');
    if (newline_pos == std::string::npos) return false;
    email_address = utf8_to_wide(trim_string(content.substr(0, newline_pos)));
    app_password  = utf8_to_wide(trim_string(content.substr(newline_pos + 1)));
    return !email_address.empty() && !app_password.empty();
}

// ─── UI control IDs ──────────────────────────────────────────────────────────
#define IDC_FOLDER_TREE   1001
#define IDC_MAIL_LIST     1002
#define IDC_BODY_EDIT     1003
#define IDC_STATUS_BAR    1004
#define IDC_HEADER_FROM   1005
#define IDC_HEADER_SUBJ   1006
#define IDC_HEADER_DATE   1007
#define IDC_BTN_REPLY     1008
#define IDC_BTN_REPLY_ALL 1009
#define IDC_BTN_FORWARD   1010
#define IDC_BTN_COMPOSE   1011
#define IDC_BTN_REFRESH   1012
#define IDC_ATTACH_LIST   1013
#define IDC_BTN_DELETE    1014
#define IDC_BTN_SETTINGS  1015
// Compose window
#define IDC_COMP_TO       2001
#define IDC_COMP_CC       2002
#define IDC_COMP_SUBJ     2003
#define IDC_COMP_BODY     2004
#define IDC_COMP_SEND     2005
#define IDC_COMP_CANCEL   2006
// Login dialog
#define IDC_LOGIN_EMAIL   3001
#define IDC_LOGIN_PASS    3002
#define IDC_LOGIN_OK      3003
#define IDC_LOGIN_CANCEL  3004
// Folder context menu commands
#define IDM_NEW_FOLDER    4001
#define IDM_RENAME_FOLDER 4002
#define IDM_DELETE_FOLDER 4003

// ─── Compose window ───────────────────────────────────────────────────────────
struct ComposeWindow {
    HWND window;
    HWND edit_to;
    HWND edit_cc;
    HWND edit_subject;
    HWND edit_body;
    HWND main_window; // parent to notify
};

static ComposeWindow* g_compose = nullptr;

static LRESULT CALLBACK compose_wnd_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CREATE) {
        CREATESTRUCT* create_struct = (CREATESTRUCT*)lparam;
        ComposeWindow* compose = (ComposeWindow*)create_struct->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)compose);

        HFONT default_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        auto make_label = [&](const wchar_t* label_text, int label_x, int label_y, int label_w) {
            HWND label = CreateWindowW(L"STATIC", label_text, WS_CHILD | WS_VISIBLE,
                label_x, label_y + 3, label_w, 20, window, nullptr, nullptr, nullptr);
            SendMessageW(label, WM_SETFONT, (WPARAM)default_font, TRUE);
        };
        auto make_edit = [&](int edit_id, int edit_x, int edit_y, int edit_w, int edit_h, bool multiline) -> HWND {
            DWORD edit_style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;
            if (multiline) edit_style |= ES_MULTILINE | WS_VSCROLL | ES_AUTOVSCROLL;
            HWND edit = CreateWindowW(L"EDIT", L"", edit_style,
                edit_x, edit_y, edit_w, edit_h, window, (HMENU)(UINT_PTR)edit_id, nullptr, nullptr);
            SendMessageW(edit, WM_SETFONT, (WPARAM)default_font, TRUE);
            return edit;
        };

        make_label(L"To:", 8, 8, 40);
        compose->edit_to = make_edit(IDC_COMP_TO, 50, 8, 540, 22, false);
        make_label(L"Cc:", 8, 36, 40);
        compose->edit_cc = make_edit(IDC_COMP_CC, 50, 36, 540, 22, false);
        make_label(L"Subject:", 8, 64, 55);
        compose->edit_subject = make_edit(IDC_COMP_SUBJ, 65, 64, 525, 22, false);

        compose->edit_body = make_edit(IDC_COMP_BODY, 8, 96, 584, 350, true);

        HWND btn_send = CreateWindowW(L"BUTTON", L"Send", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            8, 454, 80, 28, window, (HMENU)IDC_COMP_SEND, nullptr, nullptr);
        SendMessageW(btn_send, WM_SETFONT, (WPARAM)default_font, TRUE);
        HWND btn_cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            96, 454, 80, 28, window, (HMENU)IDC_COMP_CANCEL, nullptr, nullptr);
        SendMessageW(btn_cancel, WM_SETFONT, (WPARAM)default_font, TRUE);

        return 0;
    }

    ComposeWindow* compose = (ComposeWindow*)GetWindowLongPtrW(window, GWLP_USERDATA);

    if (message == WM_COMMAND) {
        int control_id = LOWORD(wparam);
        if (control_id == IDC_COMP_CANCEL) {
            DestroyWindow(window);
        } else if (control_id == IDC_COMP_SEND) {
            ComposeFields* fields = new ComposeFields();
            wchar_t text_buf[4096];
            GetWindowTextW(compose->edit_to,      text_buf, 4096); fields->to_addresses  = text_buf;
            GetWindowTextW(compose->edit_cc,      text_buf, 4096); fields->cc_addresses  = text_buf;
            GetWindowTextW(compose->edit_subject, text_buf, 4096); fields->subject_text  = text_buf;
            GetWindowTextW(compose->edit_body,    text_buf, 4096); fields->body_text     = text_buf;
            fields->from_address = g_app.email_address;

            HWND main_window = compose->main_window;
            std::thread([fields, main_window]() {
                SmtpClient smtp;
                std::string email_utf8 = wide_to_utf8(g_app.email_address);
                std::string pass_utf8  = wide_to_utf8(g_app.app_password);
                bool success = smtp.send_message(email_utf8, pass_utf8, *fields);
                delete fields;
                std::wstring* status_text = new std::wstring(success ? L"Message sent." : utf8_to_wide("Send failed: " + smtp.last_error_));
                PostMessageW(main_window, WM_SMTP_DONE, success ? 0 : 1, (LPARAM)status_text);
            }).detach();

            DestroyWindow(window);
        }
        return 0;
    }
    if (message == WM_DESTROY) {
        if (g_compose && g_compose->window == window) {
            delete g_compose;
            g_compose = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static void open_compose_window(HWND parent_window, const std::wstring& initial_to = L"",
                                const std::wstring& initial_subject = L"",
                                const std::wstring& initial_body = L"") {
    if (g_compose) {
        SetForegroundWindow(g_compose->window);
        return;
    }
    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSW wnd_class = {};
        wnd_class.lpfnWndProc   = compose_wnd_proc;
        wnd_class.hInstance     = GetModuleHandleW(nullptr);
        wnd_class.lpszClassName = L"iCloudCompose";
        wnd_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wnd_class.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wnd_class);
        class_registered = true;
    }

    g_compose = new ComposeWindow();
    g_compose->main_window = parent_window;

    HWND compose_hwnd = CreateWindowW(L"iCloudCompose", L"Compose Message",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        200, 150, 614, 520, parent_window, nullptr, GetModuleHandleW(nullptr), g_compose);
    g_compose->window = compose_hwnd;

    if (!initial_to.empty())      SetWindowTextW(g_compose->edit_to,      initial_to.c_str());
    if (!initial_subject.empty()) SetWindowTextW(g_compose->edit_subject,  initial_subject.c_str());
    if (!initial_body.empty())    SetWindowTextW(g_compose->edit_body,     initial_body.c_str());
}

// ─── Login Dialog ─────────────────────────────────────────────────────────────
static INT_PTR CALLBACK login_dlg_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        SetWindowTextW(GetDlgItem(dialog, IDC_LOGIN_EMAIL), g_app.email_address.c_str());
        SetWindowTextW(GetDlgItem(dialog, IDC_LOGIN_PASS),  g_app.app_password.c_str());
        return TRUE;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wparam) == IDC_LOGIN_OK || LOWORD(wparam) == IDOK) {
            wchar_t text_buf[512];
            GetDlgItemTextW(dialog, IDC_LOGIN_EMAIL, text_buf, 512);
            g_app.email_address = text_buf;
            GetDlgItemTextW(dialog, IDC_LOGIN_PASS, text_buf, 512);
            g_app.app_password = text_buf;
            save_config(g_app.email_address, g_app.app_password);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wparam) == IDC_LOGIN_CANCEL || LOWORD(wparam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

static void show_login_dialog(HWND parent_window) {
    // Build dialog template in memory
    std::vector<BYTE> dialog_template(1024, 0);
    DLGTEMPLATE* dlg_tmpl = (DLGTEMPLATE*)dialog_template.data();
    dlg_tmpl->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    dlg_tmpl->x = 0; dlg_tmpl->y = 0;
    dlg_tmpl->cx = 220; dlg_tmpl->cy = 120;
    dlg_tmpl->cdit = 6; // number of controls

    // After DLGTEMPLATE: menu (WORD=0), class (WORD=0), title
    BYTE* ptr = dialog_template.data() + sizeof(DLGTEMPLATE);
    *(WORD*)ptr = 0; ptr += 2; // no menu
    *(WORD*)ptr = 0; ptr += 2; // default class
    // title: "iCloud Login"
    const wchar_t title_text[] = L"iCloud Login";
    memcpy(ptr, title_text, (wcslen(title_text) + 1) * 2); ptr += (wcslen(title_text) + 1) * 2;

    // align to DWORD
    while ((ptr - dialog_template.data()) % 4) ptr++;

    auto add_control = [&](DWORD ctrl_style, short ctrl_x, short ctrl_y, short ctrl_w, short ctrl_h,
                           WORD ctrl_id, const wchar_t* ctrl_class, const wchar_t* ctrl_text) {
        while ((ptr - dialog_template.data()) % 4) ptr++;
        DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)ptr;
        item->style = ctrl_style | WS_CHILD | WS_VISIBLE;
        item->x = ctrl_x; item->y = ctrl_y;
        item->cx = ctrl_w; item->cy = ctrl_h;
        item->id = ctrl_id;
        ptr += sizeof(DLGITEMTEMPLATE);
        *(WORD*)ptr = 0xFFFF; ptr += 2;
        // class atom
        if (wcscmp(ctrl_class, L"BUTTON") == 0)     { *(WORD*)ptr = 0x0080; }
        else if (wcscmp(ctrl_class, L"EDIT") == 0)  { *(WORD*)ptr = 0x0081; }
        else if (wcscmp(ctrl_class, L"STATIC") == 0){ *(WORD*)ptr = 0x0082; }
        ptr += 2;
        memcpy(ptr, ctrl_text, (wcslen(ctrl_text) + 1) * 2); ptr += (wcslen(ctrl_text) + 1) * 2;
        *(WORD*)ptr = 0; ptr += 2; // no extra data
    };

    add_control(SS_LEFT, 8, 12, 60, 12, 0xFFFF, L"STATIC", L"Email:");
    add_control(WS_BORDER | ES_AUTOHSCROLL, 72, 10, 140, 14, IDC_LOGIN_EMAIL, L"EDIT", L"");
    add_control(SS_LEFT, 8, 34, 60, 12, 0xFFFF, L"STATIC", L"App Password:");
    add_control(WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL, 72, 32, 140, 14, IDC_LOGIN_PASS, L"EDIT", L"");
    add_control(BS_DEFPUSHBUTTON, 60, 60, 50, 14, IDC_LOGIN_OK, L"BUTTON", L"OK");
    add_control(BS_PUSHBUTTON, 118, 60, 50, 14, IDC_LOGIN_CANCEL, L"BUTTON", L"Cancel");

    DialogBoxIndirectW(GetModuleHandleW(nullptr), (DLGTEMPLATE*)dialog_template.data(),
                       parent_window, login_dlg_proc);
}

// ─── Main Window ──────────────────────────────────────────────────────────────
struct MainWindow {
    HWND window;
    HWND tree_view;
    HWND list_view;
    HWND edit_body;
    HWND status_bar;
    HWND label_from;
    HWND label_subject;
    HWND label_date;
    HWND btn_reply;
    HWND btn_reply_all;
    HWND btn_forward;
    HWND btn_compose;
    HWND btn_refresh;
    HWND btn_delete;
    HWND btn_settings;
    HWND label_attachments;
    HWND attach_list;
    HWND account_bar;    // panel showing account buttons at top of sidebar
    HWND progress_bar;   // thin progress bar for bulk operations
};

static MainWindow g_main;
static HWND g_splash_window = nullptr;
static HFONT g_body_font = nullptr;
static HFONT g_bold_font = nullptr;
static WNDPROC g_list_view_original_proc = nullptr;

// Draggable vertical splitter between mail-list and body pane
static int  g_splitter_x      = 564;  // pixels from left edge; matches initial right_x
static bool g_splitter_drag   = false;
static HCURSOR g_cursor_sizewe = nullptr;

// Background prefetch state
static std::atomic<bool> g_prefetch_running{false};
static std::atomic<bool> g_prefetch_stop{false};
static std::atomic<int>  g_prefetch_done{0};
static std::atomic<int>  g_prefetch_total{0};
// Set to true when the user is waiting for an email — prefetch pauses immediately
static std::atomic<bool> g_user_fetch_pending{false};

// Sort state: column index (0=From,1=Subject,2=Date) and direction.
// -1 means unsorted (natural server order = newest first by UID).
static int  g_sort_column    = -1;
static bool g_sort_ascending = true;

static void set_status(const std::wstring& status_text) {
    SetWindowTextW(g_main.status_bar, status_text.c_str());
}

static void post_status(HWND window, const std::wstring& status_text) {
    std::wstring* text_copy = new std::wstring(status_text);
    PostMessageW(window, WM_STATUS_TEXT, 0, (LPARAM)text_copy);
}

// Recursive helper: get or create a tree node for full_path, creating ancestors as needed.
static HTREEITEM get_or_create_tree_node(
    const std::string& full_path,
    const std::set<std::string>& folder_set,
    std::map<std::string, HTREEITEM>& item_map)
{
    auto found = item_map.find(full_path);
    if (found != item_map.end()) return found->second;

    HTREEITEM parent_item = TVI_ROOT;
    std::string display_name = full_path;
    size_t slash = full_path.rfind('/');
    if (slash != std::string::npos) {
        std::string parent_path = full_path.substr(0, slash);
        display_name = full_path.substr(slash + 1);
        parent_item = get_or_create_tree_node(parent_path, folder_set, item_map);
    }

    LPARAM lp;
    if (folder_set.count(full_path)) {
        lp = (LPARAM)g_folder_paths.size();
        g_folder_paths.push_back(full_path);
    } else {
        lp = (LPARAM)-1; // intermediate container — not a real IMAP folder
    }

    TVINSERTSTRUCT tvis = {};
    tvis.hParent      = parent_item;
    tvis.hInsertAfter = TVI_SORT;
    tvis.item.mask    = TVIF_TEXT | TVIF_PARAM;
    std::wstring wide_display = utf8_to_wide(display_name);
    tvis.item.pszText = (LPWSTR)wide_display.c_str();
    tvis.item.lParam  = lp;
    HTREEITEM handle = TreeView_InsertItem(g_main.tree_view, &tvis);
    item_map[full_path] = handle;
    return handle;
}

static void populate_folder_tree(const std::vector<std::string>& folders) {
    TreeView_DeleteAllItems(g_main.tree_view);
    g_folder_paths.clear();

    std::set<std::string> folder_set(folders.begin(), folders.end());
    std::map<std::string, HTREEITEM> item_map;

    // INBOX always at the top
    HTREEITEM inbox_item = nullptr;
    if (folder_set.count("INBOX")) {
        LPARAM lp = (LPARAM)g_folder_paths.size();
        g_folder_paths.push_back("INBOX");
        TVINSERTSTRUCT tvis = {};
        tvis.hParent      = TVI_ROOT;
        tvis.hInsertAfter = TVI_FIRST;
        tvis.item.mask    = TVIF_TEXT | TVIF_PARAM;
        tvis.item.pszText = (LPWSTR)L"INBOX";
        tvis.item.lParam  = lp;
        inbox_item = TreeView_InsertItem(g_main.tree_view, &tvis);
        item_map["INBOX"] = inbox_item;
    }

    // Sort remaining folders so parents always appear before children
    std::vector<std::string> sorted;
    for (const std::string& folder : folders) {
        if (folder != "INBOX") sorted.push_back(folder);
    }
    std::sort(sorted.begin(), sorted.end());

    for (const std::string& folder : sorted) {
        get_or_create_tree_node(folder, folder_set, item_map);
    }

    if (inbox_item) {
        TreeView_SelectItem(g_main.tree_view, inbox_item);
        TreeView_Expand(g_main.tree_view, inbox_item, TVE_EXPAND);
    }
    // Expand first-level nodes so newly created subfolders are visible
    HTREEITEM root_child = TreeView_GetChild(g_main.tree_view, TVI_ROOT);
    while (root_child) {
        TreeView_Expand(g_main.tree_view, root_child, TVE_EXPAND);
        root_child = TreeView_GetNextSibling(g_main.tree_view, root_child);
    }
}

static void populate_mail_list(const std::vector<MailHeader>& headers); // forward

// Update sort-arrow indicators on ListView column headers.
static void update_sort_arrow(int sorted_column, bool ascending) {
    HWND header_ctrl = ListView_GetHeader(g_main.list_view);
    if (!header_ctrl) return;
    for (int col_index = 0; col_index < 3; ++col_index) {
        HDITEMW header_item = {};
        header_item.mask = HDI_FORMAT;
        Header_GetItem(header_ctrl, col_index, &header_item);
        header_item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (col_index == sorted_column) {
            header_item.fmt |= ascending ? HDF_SORTUP : HDF_SORTDOWN;
        }
        Header_SetItem(header_ctrl, col_index, &header_item);
    }
}

// Sort g_app.current_headers by the given column, then repopulate the list.
static void sort_and_repopulate(int column, bool ascending) {
    auto comparator = [column, ascending](const MailHeader& first, const MailHeader& second) -> bool {
        int result = 0;
        if (column == 0) {
            result = _stricmp(first.from_address.c_str(), second.from_address.c_str());
        } else if (column == 1) {
            result = _stricmp(first.subject_text.c_str(), second.subject_text.c_str());
        } else {
            // Column 2: sort by UID (monotonically increasing ≈ send date).
            // Safe stoul with fallback to 0 for empty strings.
            unsigned long uid_first  = first.uid.empty()  ? 0 : std::stoul(first.uid);
            unsigned long uid_second = second.uid.empty() ? 0 : std::stoul(second.uid);
            result = (uid_first < uid_second) ? -1 : (uid_first > uid_second) ? 1 : 0;
        }
        return ascending ? result < 0 : result > 0;
    };
    std::stable_sort(g_app.current_headers.begin(), g_app.current_headers.end(), comparator);
    populate_mail_list(g_app.current_headers);
    update_sort_arrow(column, ascending);
}

static void populate_mail_list(const std::vector<MailHeader>& headers) {
    ListView_DeleteAllItems(g_main.list_view);
    int row_index = 0;
    for (const MailHeader& header : headers) {
        LVITEMW list_item = {};
        list_item.mask  = LVIF_TEXT | LVIF_PARAM;
        list_item.iItem = row_index;
        std::wstring from_wide = utf8_to_wide(header.from_address);
        list_item.pszText = (LPWSTR)from_wide.c_str();
        list_item.lParam  = (LPARAM)row_index;
        ListView_InsertItem(g_main.list_view, &list_item);

        LVITEMW subj_item = {};
        subj_item.mask     = LVIF_TEXT;
        subj_item.iItem    = row_index;
        subj_item.iSubItem = 1;
        std::wstring subj_wide = utf8_to_wide(header.subject_text);
        subj_item.pszText = (LPWSTR)subj_wide.c_str();
        ListView_SetItem(g_main.list_view, &subj_item);

        LVITEMW date_item = {};
        date_item.mask     = LVIF_TEXT;
        date_item.iItem    = row_index;
        date_item.iSubItem = 2;
        std::wstring date_wide = utf8_to_wide(header.date_string);
        date_item.pszText = (LPWSTR)date_wide.c_str();
        ListView_SetItem(g_main.list_view, &date_item);

        ++row_index;
    }
}

static void append_to_mail_list(const std::vector<MailHeader>& headers) {
    int row_index = ListView_GetItemCount(g_main.list_view);
    for (const MailHeader& header : headers) {
        LVITEMW list_item = {};
        list_item.mask    = LVIF_TEXT | LVIF_PARAM;
        list_item.iItem   = row_index;
        std::wstring from_wide = utf8_to_wide(header.from_address);
        list_item.pszText = (LPWSTR)from_wide.c_str();
        list_item.lParam  = (LPARAM)row_index;
        ListView_InsertItem(g_main.list_view, &list_item);

        LVITEMW subj_item = {};
        subj_item.mask = LVIF_TEXT; subj_item.iItem = row_index; subj_item.iSubItem = 1;
        std::wstring subj_wide = utf8_to_wide(header.subject_text);
        subj_item.pszText = (LPWSTR)subj_wide.c_str();
        ListView_SetItem(g_main.list_view, &subj_item);

        LVITEMW date_item = {};
        date_item.mask = LVIF_TEXT; date_item.iItem = row_index; date_item.iSubItem = 2;
        std::wstring date_wide = utf8_to_wide(header.date_string);
        date_item.pszText = (LPWSTR)date_wide.c_str();
        ListView_SetItem(g_main.list_view, &date_item);

        ++row_index;
    }
}

static void start_connect(HWND window) {
    set_status(L"Connecting...");
    std::thread([window]() {
        try {
            g_app.imap_client = std::make_unique<ImapClient>();
            std::string email_utf8 = wide_to_utf8(g_app.email_address);
            std::string pass_utf8  = wide_to_utf8(g_app.app_password);

            post_status(window, L"Logging in to imap.mail.me.com...");
            if (!g_app.imap_client->connect(email_utf8, pass_utf8)) {
                post_status(window, utf8_to_wide("Login failed: " + g_app.imap_client->last_error));
                return;
            }
            g_app.imap_connected = true;
            post_status(window, L"Connected. Loading folders...");

            // Open a second IMAP connection dedicated to background prefetch so
            // the main connection is always free for user interactions.
            g_app.prefetch_client = std::make_unique<ImapClient>();
            if (g_app.prefetch_client->connect(email_utf8, pass_utf8))
                g_app.prefetch_connected = true;
            else
                g_app.prefetch_client.reset();

            auto folders = g_app.imap_client->list_folders();
            auto* folder_vec = new std::vector<std::string>(folders);
            PostMessageW(window, WM_IMAP_FOLDERS, (WPARAM)folder_vec, 0);
            // Selecting INBOX in the tree will trigger load_folder automatically.
        } catch (const std::exception& exception_error) {
            post_status(window, utf8_to_wide(std::string("Error: ") + exception_error.what()));
        } catch (...) {
            post_status(window, L"Unexpected error in connect thread");
        }
    }).detach();
}

static void load_folder(HWND window, const std::string& folder_name) {
    if (folder_name.empty()) return;
    g_app.current_folder = folder_name;
    // Reset sort state when switching folders
    g_sort_column    = -1;
    g_sort_ascending = true;
    update_sort_arrow(-1, true);
    int generation = ++g_load_generation;
    ListView_DeleteAllItems(g_main.list_view);
    post_status(window, utf8_to_wide("Loading " + folder_name + "..."));

    std::thread([window, folder_name, generation]() {
        try {
            if (!g_app.imap_client || !g_app.imap_connected) return;

            // Use the prefetch client for folder loading so the main imap_client
            // stays free for instant user email opens (no mutex contention).
            ImapClient* folder_client = (g_app.prefetch_client && g_app.prefetch_connected)
                ? g_app.prefetch_client.get() : g_app.imap_client.get();

            // Fast path: load cached headers for instant display before IMAP round-trip
            {
                std::vector<MailHeader> cached_headers;
                std::string cache_key = g_app.imap_client->get_email_address();
                if (cache_read_headers(folder_name, cache_key, cached_headers) &&
                    g_load_generation == generation) {
                    auto* fast_headers = new std::vector<MailHeader>(std::move(cached_headers));
                    PostMessageW(window, WM_IMAP_HEADERS, (WPARAM)fast_headers, 0);
                    debug_log("HDR CACHE: instant load " +
                              std::to_string(fast_headers->size()) +
                              " headers for " + folder_name);
                }
            }

            // SELECT + SEARCH in one locked call
            auto all_uids = folder_client->select_and_get_uids(folder_name);
            if (g_load_generation != generation) return;

            const size_t batch_size = 50;
            bool first_batch = true;
            for (size_t offset = 0; offset < all_uids.size(); offset += batch_size) {
                if (g_load_generation != generation) return; // user switched folder

                size_t end = std::min(offset + batch_size, all_uids.size());
                std::vector<std::string> batch_uids(all_uids.begin() + offset,
                                                    all_uids.begin() + end);
                auto batch_headers = folder_client->fetch_headers_batch(batch_uids);
                if (g_load_generation != generation) return;

                auto* header_ptr = new std::vector<MailHeader>(std::move(batch_headers));
                UINT message_id = first_batch ? WM_IMAP_HEADERS : WM_IMAP_HEADERS_APPEND;
                PostMessageW(window, message_id, (WPARAM)header_ptr, 0);
                first_batch = false;
            }
            if (all_uids.empty()) PostMessageW(window, WM_IMAP_HEADERS, (WPARAM)new std::vector<MailHeader>(), 0);

            if (g_load_generation == generation) {
                post_status(window, utf8_to_wide(folder_name + " — " +
                    std::to_string(all_uids.size()) + " messages"));
            }
        } catch (const std::exception& exception_error) {
            post_status(window, utf8_to_wide(std::string("Error: ") + exception_error.what()));
        } catch (...) {
            post_status(window, L"Unexpected error in load_folder thread");
        }
    }).detach();
}

static void load_message(HWND window, int row_index) {
    if (row_index < 0 || row_index >= (int)g_app.current_headers.size()) return;
    std::string uid = g_app.current_headers[row_index].uid;
    g_app.current_uid     = uid;
    g_app.current_from    = g_app.current_headers[row_index].from_address;
    g_app.current_subject = g_app.current_headers[row_index].subject_text;

    // Update header labels immediately
    SetWindowTextW(g_main.label_from,    utf8_to_wide(g_app.current_from).c_str());
    SetWindowTextW(g_main.label_subject, utf8_to_wide(g_app.current_subject).c_str());
    SetWindowTextW(g_main.label_date,    utf8_to_wide(g_app.current_headers[row_index].date_string).c_str());
    if (g_browser_host) g_browser_host->navigate_text("Loading...");

    post_status(window, L"Fetching message...");
    std::string folder_snapshot = g_app.current_folder;
    // Signal prefetch to pause immediately so this request gets the mutex first
    g_user_fetch_pending = true;
    std::thread([window, uid, folder_snapshot, row_index]() {
        try {
            if (!g_app.imap_client || !g_app.imap_connected) {
                g_user_fetch_pending = false;
                return;
            }
            std::string body_text = g_app.imap_client->fetch_body_smart(folder_snapshot, uid);
            g_user_fetch_pending = false;  // prefetch may resume
            auto attachments = g_app.imap_client->take_last_attachments();
            std::string html_raw = g_app.imap_client->take_last_html();
            g_app.current_body = body_text;
            g_app.current_body_html = html_raw;
            auto* result = new BodyResult{std::move(body_text), std::move(html_raw), std::move(attachments)};
            PostMessageW(window, WM_IMAP_BODY, (WPARAM)result, 0);
            g_app.imap_client->mark_seen(folder_snapshot, uid);
            PostMessageW(window, WM_IMAP_READ_UPDATE, (WPARAM)row_index, 0);
            post_status(window, L"Ready.");
        } catch (const std::exception& exception_error) {
            g_user_fetch_pending = false;
            post_status(window, utf8_to_wide(std::string("Error: ") + exception_error.what()));
        } catch (...) {
            g_user_fetch_pending = false;
            post_status(window, L"Unexpected error in load_message thread");
        }
    }).detach();
}

static void start_background_prefetch(const std::string& folder,
                                      const std::vector<MailHeader>& headers,
                                      HWND status_window) {
    // Stop any previous prefetch
    g_prefetch_stop = true;

    // Collect UIDs that are NOT yet cached (fast check, no IMAP)
    std::vector<std::string> uncached_uids;
    for (const auto& header : headers) {
        if (!cache_exists(folder, header.uid))
            uncached_uids.push_back(header.uid);
    }
    if (uncached_uids.empty()) {
        post_status(status_window, utf8_to_wide(folder + " — all " +
            std::to_string(headers.size()) + " messages cached"));
        return;
    }

    g_prefetch_done  = 0;
    g_prefetch_total = (int)uncached_uids.size();
    g_prefetch_stop  = false;

    std::thread([folder, uncached_uids, status_window]() {
        g_prefetch_running = true;

        for (int index = 0; index < (int)uncached_uids.size(); ++index) {
            if (g_prefetch_stop) break;

            const std::string& uid = uncached_uids[index];

            // Update status bar
            int done  = index + 1;
            int total = (int)uncached_uids.size();
            post_status(status_window,
                        utf8_to_wide("Caching " + std::to_string(done) +
                                     "/" + std::to_string(total) + "…"));

            if (g_prefetch_stop) break;

            try {
                // Reconnect prefetch_client if it was dropped by the server
                if (g_app.prefetch_client && !g_app.prefetch_connected) {
                    std::string email = g_app.imap_client ? g_app.imap_client->get_email_address() : "";
                    std::string pass  = wide_to_utf8(g_app.app_password);
                    if (!email.empty() && g_app.prefetch_client->connect(email, pass)) {
                        g_app.prefetch_connected = true;
                        debug_log("PREFETCH: reconnected");
                    }
                }
                // Use the dedicated prefetch connection so the main connection
                // stays free for instant user interactions.
                ImapClient* client = (g_app.prefetch_client && g_app.prefetch_connected)
                    ? g_app.prefetch_client.get()
                    : (g_app.imap_client && g_app.imap_connected ? g_app.imap_client.get() : nullptr);
                if (client) {
                    client->fetch_body_smart(folder, uid, true);
                    // If connection was lost during fetch, mark for reconnect
                    if (g_app.prefetch_client && client == g_app.prefetch_client.get()
                        && !g_app.prefetch_client->is_connected())
                        g_app.prefetch_connected = false;
                }
            } catch (...) {}

            g_prefetch_done = done;

            if (!g_prefetch_stop) Sleep(50);
        }

        if (!g_prefetch_stop) {
            post_status(status_window,
                        utf8_to_wide(folder + " — " +
                                     std::to_string(uncached_uids.size()) +
                                     " messages cached"));
        }
        g_prefetch_running = false;
    }).detach();
}

static LRESULT CALLBACK list_view_subclass_proc(HWND list_wnd, UINT message,
                                                WPARAM wparam, LPARAM lparam) {
    if (message == WM_KEYDOWN && wparam == VK_DELETE) {
        SendMessageW(g_main.window, WM_COMMAND, IDC_BTN_DELETE, 0);
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == 'A' &&
        (GetKeyState(VK_CONTROL) & 0x8000)) {
        int count = ListView_GetItemCount(list_wnd);
        for (int index = 0; index < count; ++index)
            ListView_SetItemState(list_wnd, index,
                LVIS_SELECTED, LVIS_SELECTED);
        return 0;
    }
    return CallWindowProcW(g_list_view_original_proc, list_wnd, message, wparam, lparam);
}

static void delete_selected_messages() {
    if (!g_app.imap_connected || !g_app.imap_client) return;

    // Collect all selected UIDs and row indices
    std::vector<std::pair<int, std::string>> selected;  // (row_index, uid)
    int index = -1;
    while ((index = ListView_GetNextItem(g_main.list_view, index, LVNI_SELECTED)) != -1) {
        if (index < (int)g_app.current_headers.size())
            selected.push_back({index, g_app.current_headers[index].uid});
    }
    if (selected.empty()) return;

    std::wstring confirm_msg = L"Delete " + std::to_wstring(selected.size()) +
                               L" message(s)?";
    if (MessageBoxW(nullptr, confirm_msg.c_str(), L"Confirm",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    std::string folder = g_app.current_folder;
    std::vector<std::string> uids;
    for (const auto& item : selected) uids.push_back(item.second);

    HWND main_window = GetAncestor(g_main.list_view, GA_ROOT);
    std::thread([uids, folder, main_window]() {
        if (!g_app.imap_client || !g_app.imap_connected) return;
        int total_messages = (int)uids.size();
        ShowWindow(g_main.progress_bar, SW_SHOW);
        for (int index = 0; index < total_messages; ++index) {
            g_app.imap_client->delete_message(folder, uids[index]);
            int percent = (index + 1) * 100 / total_messages;
            PostMessageW(main_window, WM_DELETE_PROGRESS, (WPARAM)percent, 0);
        }
        // Re-post a refresh to reload the folder
        PostMessageW(main_window, WM_COMMAND, IDC_BTN_REFRESH, 0);
    }).detach();
}

static void save_and_open_attachment(AttachmentInfo& attachment) {
    // On-demand download if not yet fetched
    if (attachment.raw_bytes.empty() && !attachment.section_number.empty()) {
        if (!g_app.imap_client || !g_app.imap_connected) return;
        post_status(GetAncestor(g_main.attach_list, GA_ROOT), L"Downloading attachment...");
        // Determine encoding: use stored encoding, default base64 for binary
        std::string enc = attachment.encoding.empty() ? "base64" : attachment.encoding;
        if (enc == "7bit" || enc == "8bit") {
            // text-like encoding — keep as-is but still might be binary content
            if (attachment.content_type.find("text") == std::string::npos) enc = "base64";
        }
        try {
            auto raw_vec = g_app.imap_client->fetch_attachment_section(
                attachment.folder_name, attachment.uid, attachment.section_number, enc);
            attachment.raw_bytes.assign(raw_vec.begin(), raw_vec.end());
        } catch (...) {
            MessageBoxW(nullptr, L"Failed to download attachment.", L"Error", MB_OK | MB_ICONERROR);
            return;
        }
    }
    if (attachment.raw_bytes.empty()) {
        MessageBoxW(nullptr, L"Attachment data not available.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }
    wchar_t temp_path[MAX_PATH];
    GetTempPathW(MAX_PATH, temp_path);
    std::wstring att_dir = std::wstring(temp_path) + L"icloud_att\\";
    CreateDirectoryW(att_dir.c_str(), nullptr);
    std::wstring filepath = att_dir + utf8_to_wide(attachment.filename);
    HANDLE file_handle = CreateFileW(filepath.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) {
        MessageBoxW(nullptr, L"Failed to save attachment", L"Error", MB_OK | MB_ICONERROR);
        return;
    }
    DWORD written;
    WriteFile(file_handle, attachment.raw_bytes.data(), (DWORD)attachment.raw_bytes.size(),
              &written, nullptr);
    CloseHandle(file_handle);
    ShellExecuteW(nullptr, L"open", filepath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Show a simple modal input dialog. Returns the entered text, or empty string on cancel.
// Modal: disables parent while open, handles Enter=OK and Escape=Cancel.
static std::wstring show_input_dialog(HWND parent_window, const std::wstring& title,
                                      const std::wstring& prompt,
                                      const std::wstring& default_value = L"") {
    struct InputState {
        std::wstring result;
        bool         done;
        bool         ok;
        HWND         edit_control;
        std::wstring prompt_text;
    };

    static InputState state;
    state = { L"", false, false, nullptr, prompt };

    static ATOM dialog_class_atom = 0;
    static auto dialog_wnd_proc = [](HWND wnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        auto* dialog_state = (InputState*)GetWindowLongPtrW(wnd, GWLP_USERDATA);

        switch (msg) {
        case WM_CREATE: {
            auto* create_struct = (CREATESTRUCTW*)lp;
            auto* create_state  = (InputState*)create_struct->lpCreateParams;
            SetWindowLongPtrW(wnd, GWLP_USERDATA, (LPARAM)create_state);

            HFONT dialog_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HINSTANCE module_instance = (HINSTANCE)GetWindowLongPtrW(wnd, GWLP_HINSTANCE);

            // Prompt label
            HWND label_control = CreateWindowW(L"STATIC", create_state->prompt_text.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                10, 10, 280, 18, wnd, nullptr, module_instance, nullptr);
            SendMessageW(label_control, WM_SETFONT, (WPARAM)dialog_font, TRUE);

            // Edit control
            create_state->edit_control = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                10, 32, 280, 22, wnd, (HMENU)(UINT_PTR)101, module_instance, nullptr);
            SendMessageW(create_state->edit_control, WM_SETFONT, (WPARAM)dialog_font, TRUE);

            // OK button
            HWND ok_button = CreateWindowW(L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                60, 62, 80, 26, wnd, (HMENU)(UINT_PTR)IDOK, module_instance, nullptr);
            SendMessageW(ok_button, WM_SETFONT, (WPARAM)dialog_font, TRUE);

            // Cancel button
            HWND cancel_button = CreateWindowW(L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                150, 62, 80, 26, wnd, (HMENU)(UINT_PTR)IDCANCEL, module_instance, nullptr);
            SendMessageW(cancel_button, WM_SETFONT, (WPARAM)dialog_font, TRUE);

            SetFocus(create_state->edit_control);
            return 0;
        }
        case WM_COMMAND:
            if (!dialog_state) break;
            if (LOWORD(wp) == IDOK) {
                wchar_t text_buf[512] = {};
                GetWindowTextW(dialog_state->edit_control, text_buf, 512);
                dialog_state->result = text_buf;
                dialog_state->ok     = true;
                dialog_state->done   = true;
                DestroyWindow(wnd);
                return 0;
            }
            if (LOWORD(wp) == IDCANCEL) {
                dialog_state->ok   = false;
                dialog_state->done = true;
                DestroyWindow(wnd);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wp == VK_RETURN && dialog_state) {
                SendMessageW(wnd, WM_COMMAND, IDOK, 0);
                return 0;
            }
            if (wp == VK_ESCAPE && dialog_state) {
                SendMessageW(wnd, WM_COMMAND, IDCANCEL, 0);
                return 0;
            }
            break;
        case WM_DESTROY:
            if (dialog_state) dialog_state->done = true;
            // Do NOT call PostQuitMessage — that would kill the main message loop.
            return 0;
        }
        return DefWindowProcW(wnd, msg, wp, lp);
    };

    // Register the window class once
    if (dialog_class_atom == 0) {
        WNDCLASSEXW dialog_wc = {};
        dialog_wc.cbSize        = sizeof(dialog_wc);
        dialog_wc.lpfnWndProc   = (WNDPROC)+dialog_wnd_proc;
        dialog_wc.hInstance     = GetModuleHandleW(nullptr);
        dialog_wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        dialog_wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        dialog_wc.lpszClassName = L"FastMailInputDlg";
        dialog_class_atom = RegisterClassExW(&dialog_wc);
    }

    int screen_width  = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);
    int dialog_width  = 312;
    int dialog_height = 112;

    HWND dialog_hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"FastMailInputDlg", title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        (screen_width  - dialog_width)  / 2,
        (screen_height - dialog_height) / 2,
        dialog_width, dialog_height,
        parent_window, nullptr, GetModuleHandleW(nullptr), &state);

    if (!dialog_hwnd) return L"";

    // Set the default text in the edit control
    if (state.edit_control && !default_value.empty()) {
        SetWindowTextW(state.edit_control, default_value.c_str());
        SendMessageW(state.edit_control, EM_SETSEL, 0, -1);
    }

    // Block parent while the dialog is open (modal behaviour)
    if (parent_window) EnableWindow(parent_window, FALSE);

    ShowWindow(dialog_hwnd, SW_SHOW);
    UpdateWindow(dialog_hwnd);

    // Local message pump — runs until the dialog sets state.done.
    // Use PeekMessage so we don't block after WM_DESTROY sets done=true.
    MSG loop_message;
    while (!state.done) {
        if (PeekMessageW(&loop_message, nullptr, 0, 0, PM_REMOVE)) {
            if (loop_message.message == WM_QUIT) {
                // Re-post so the main loop also sees it (shouldn't happen, but be safe)
                PostQuitMessage((int)loop_message.wParam);
                break;
            }
            if (!IsDialogMessageW(dialog_hwnd, &loop_message)) {
                TranslateMessage(&loop_message);
                DispatchMessageW(&loop_message);
            }
        } else {
            WaitMessage();  // yield CPU while no messages pending
        }
    }

    if (parent_window) {
        EnableWindow(parent_window, TRUE);
        SetForegroundWindow(parent_window);
    }

    return state.ok ? state.result : L"";
}

static LRESULT CALLBACK main_wnd_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        HFONT default_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HINSTANCE instance = GetModuleHandleW(nullptr);

        // Body font: Segoe UI 10pt — much more readable than DEFAULT_GUI_FONT (Tahoma 8pt)
        {
            HDC screen_dc = GetDC(nullptr);
            int font_height = -MulDiv(10, GetDeviceCaps(screen_dc, LOGPIXELSY), 72);
            ReleaseDC(nullptr, screen_dc);
            g_body_font = CreateFontW(font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            if (!g_body_font) g_body_font = default_font; // fallback
        }
        // Bold variant of the list font for unread messages
        {
            HDC screen_dc = GetDC(nullptr);
            int font_height = -MulDiv(9, GetDeviceCaps(screen_dc, LOGPIXELSY), 72);
            ReleaseDC(nullptr, screen_dc);
            g_bold_font = CreateFontW(font_height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }

        // Load RichEdit
        LoadLibraryW(L"Msftedit.dll");

        // Tree view (folders)
        g_main.tree_view = CreateWindowExW(WS_EX_CLIENTEDGE, L"SysTreeView32", L"",
            WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
            0, 0, 180, 500, window, (HMENU)IDC_FOLDER_TREE, instance, nullptr);

        // List view (messages) — no LVS_SINGLESEL so Shift+click/Ctrl+click work
        g_main.list_view = CreateWindowExW(WS_EX_CLIENTEDGE, L"SysListView32", L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
            182, 0, 380, 500, window, (HMENU)IDC_MAIL_LIST, instance, nullptr);
        ListView_SetExtendedListViewStyle(g_main.list_view,
            LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_HEADERDRAGDROP);
        // Add columns
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 140; col.pszText = (LPWSTR)L"From";
        ListView_InsertColumn(g_main.list_view, 0, &col);
        col.cx = 160; col.pszText = (LPWSTR)L"Subject";
        ListView_InsertColumn(g_main.list_view, 1, &col);
        col.cx = 76; col.pszText = (LPWSTR)L"Date";
        ListView_InsertColumn(g_main.list_view, 2, &col);
        // Subclass to intercept Delete key
        g_list_view_original_proc = (WNDPROC)SetWindowLongPtrW(
            g_main.list_view, GWLP_WNDPROC, (LONG_PTR)list_view_subclass_proc);

        // Right pane: header labels
        int right_x = 564;
        g_main.label_from = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
            right_x, 4, 600, 18, window, (HMENU)IDC_HEADER_FROM, instance, nullptr);
        // label_from gets the bold font for emphasis (set after g_bold_font is created above)
        SendMessageW(g_main.label_from, WM_SETFONT, (WPARAM)g_bold_font, TRUE);

        g_main.label_subject = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
            right_x, 24, 600, 18, window, (HMENU)IDC_HEADER_SUBJ, instance, nullptr);
        SendMessageW(g_main.label_subject, WM_SETFONT, (WPARAM)default_font, TRUE);

        g_main.label_date = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
            right_x, 44, 600, 18, window, (HMENU)IDC_HEADER_DATE, instance, nullptr);
        SendMessageW(g_main.label_date, WM_SETFONT, (WPARAM)default_font, TRUE);

        // Action buttons (BS_OWNERDRAW for colorized rendering via WM_DRAWITEM)
        auto make_button = [&](const wchar_t* label_text, int btn_x, int btn_y, int btn_id) -> HWND {
            HWND btn = CreateWindowW(L"BUTTON", label_text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                btn_x, btn_y, 80, 24, window, (HMENU)(UINT_PTR)btn_id, instance, nullptr);
            SendMessageW(btn, WM_SETFONT, (WPARAM)default_font, TRUE);
            return btn;
        };
        g_main.btn_reply     = make_button(L"Reply",     right_x,       68, IDC_BTN_REPLY);
        g_main.btn_reply_all = make_button(L"Reply All", right_x + 84,  68, IDC_BTN_REPLY_ALL);
        g_main.btn_forward   = make_button(L"Forward",   right_x + 168, 68, IDC_BTN_FORWARD);
        g_main.btn_compose   = make_button(L"Compose",   right_x + 252, 68, IDC_BTN_COMPOSE);
        g_main.btn_refresh   = make_button(L"Refresh",   right_x + 336, 68, IDC_BTN_REFRESH);
        g_main.btn_delete    = make_button(L"Delete",    right_x + 420, 68, IDC_BTN_DELETE);
        g_main.btn_settings  = make_button(L"Accounts",  right_x + 510, 68, IDC_BTN_SETTINGS);

        // Body view — embedded IE WebBrowser (renders HTML emails natively)
        g_browser_host = new BrowserHost();
        g_browser_host->create(window, right_x, 98, 700, 400);
        g_main.edit_body = g_browser_host->host_window();

        // Attachment panel (hidden until an email with attachments is opened)
        g_main.label_attachments = CreateWindowW(L"STATIC", L"Attachments (double-click to open):",
            WS_CHILD | SS_LEFT,
            right_x, 0, 700, 16, window, nullptr, instance, nullptr);
        SendMessageW(g_main.label_attachments, WM_SETFONT, (WPARAM)default_font, TRUE);

        g_main.attach_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | LVS_ICON | LVS_AUTOARRANGE | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            right_x, 0, 700, 100, window, (HMENU)IDC_ATTACH_LIST, instance, nullptr);
        ListView_SetIconSpacing(g_main.attach_list, 120, 80);

        // Status bar
        g_main.status_bar = CreateWindowW(L"msctls_statusbar32", L"Ready",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, window, (HMENU)IDC_STATUS_BAR, instance, nullptr);

        // Progress bar (thin, hidden by default, shown during bulk delete)
        g_main.progress_bar = CreateWindowW(PROGRESS_CLASS, nullptr,
            WS_CHILD | PBS_SMOOTH,
            0, 0, 0, 4, window, nullptr, instance, nullptr);
        SendMessageW(g_main.progress_bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(g_main.progress_bar, PBM_SETPOS, 0, 0);
        SendMessageW(g_main.progress_bar, PBM_SETBARCOLOR, 0, (LPARAM)RGB(21, 101, 192));

        // Account bar (hidden strip above folder tree; shown when multiple accounts exist)
        g_main.account_bar = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_LEFT,
            0, 0, 180, 0, window, nullptr, instance, nullptr);

        g_main.window = window;
        g_cursor_sizewe = LoadCursor(nullptr, IDC_SIZEWE);
        // Post WM_SIZE to ensure list columns resize correctly on startup
        PostMessageW(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(1200, 700));
        return 0;
    }

    case WM_SIZE: {
        int total_width  = LOWORD(lparam);
        int total_height = HIWORD(lparam);

        RECT status_rect;
        GetWindowRect(g_main.status_bar, &status_rect);
        int status_height = status_rect.bottom - status_rect.top;
        SendMessageW(g_main.status_bar, WM_SIZE, 0, 0);

        // Progress bar at the very bottom above the status bar
        int prog_height = 4;
        MoveWindow(g_main.progress_bar, 0, total_height - status_height - prog_height,
                   total_width, prog_height, TRUE);

        int usable_height = total_height - status_height - prog_height;
        int folder_width  = 180;
        // Clamp splitter so both panels have minimum width
        if (g_splitter_x < folder_width + 200) g_splitter_x = folder_width + 200;
        if (g_splitter_x > total_width - 300)  g_splitter_x = total_width - 300;
        int list_width    = g_splitter_x - folder_width - 4;
        int right_x       = g_splitter_x;
        int right_width   = total_width - right_x;
        if (right_width < 100) right_width = 100;

        MoveWindow(g_main.tree_view, 0, 0, folder_width, usable_height, TRUE);
        MoveWindow(g_main.list_view, folder_width + 2, 0, list_width, usable_height, TRUE);

        // Resize list columns proportionally to new list_width
        {
            int col0 = list_width * 30 / 100;  // From: 30%
            int col1 = list_width * 50 / 100;  // Subject: 50%
            int col2 = list_width - col0 - col1; // Date: remainder
            HWND lv = g_main.list_view;
            LVCOLUMNW lvc = {};
            lvc.mask = LVCF_WIDTH;
            lvc.cx   = col0; ListView_SetColumn(lv, 0, &lvc);
            lvc.cx   = col1; ListView_SetColumn(lv, 1, &lvc);
            lvc.cx   = col2; ListView_SetColumn(lv, 2, &lvc);
        }

        // Right pane
        MoveWindow(g_main.label_from,    right_x, 4,  right_width, 18, TRUE);
        MoveWindow(g_main.label_subject, right_x, 24, right_width, 18, TRUE);
        MoveWindow(g_main.label_date,    right_x, 44, right_width, 18, TRUE);
        MoveWindow(g_main.btn_reply,      right_x,       68, 80, 24, TRUE);
        MoveWindow(g_main.btn_reply_all,  right_x + 84,  68, 80, 24, TRUE);
        MoveWindow(g_main.btn_forward,    right_x + 168, 68, 80, 24, TRUE);
        MoveWindow(g_main.btn_compose,    right_x + 252, 68, 80, 24, TRUE);
        MoveWindow(g_main.btn_refresh,    right_x + 336, 68, 80, 24, TRUE);
        MoveWindow(g_main.btn_delete,     right_x + 420, 68, 80, 24, TRUE);
        MoveWindow(g_main.btn_settings,   right_x + 510, 68, 80, 24, TRUE);

        // Attachment panel appears ABOVE the body view when visible
        bool attach_visible = IsWindowVisible(g_main.attach_list) == TRUE;
        int attach_panel_height = attach_visible ? 110 : 0;
        int body_top = 98 + attach_panel_height;
        int body_height = usable_height - body_top;
        if (body_height < 40) body_height = 40;

        if (attach_visible) {
            MoveWindow(g_main.label_attachments, right_x, 98,      right_width, 16,  TRUE);
            MoveWindow(g_main.attach_list,       right_x, 98 + 18, right_width, 88,  TRUE);
        }
        if (g_browser_host) g_browser_host->resize(right_x, body_top, right_width, body_height);
        else MoveWindow(g_main.edit_body, right_x, body_top, right_width, body_height, TRUE);
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC erase_dc = (HDC)wparam;
        RECT client_rc;
        GetClientRect(window, &client_rc);
        // Light background for main area
        HBRUSH bg_brush = CreateSolidBrush(RGB(245, 246, 247));
        FillRect(erase_dc, &client_rc, bg_brush);
        DeleteObject(bg_brush);
        // Solid dark header strip on right panel (avoids STATIC control artifacts)
        RECT header_rc = {g_splitter_x, 0, client_rc.right, 96};
        HBRUSH header_brush = CreateSolidBrush(RGB(30, 38, 54));
        FillRect(erase_dc, &header_rc, header_brush);
        DeleteObject(header_brush);
        // Thin accent line at bottom of header
        HPEN accent = CreatePen(PS_SOLID, 1, RGB(21, 101, 192));
        HPEN old_acc = (HPEN)SelectObject(erase_dc, accent);
        MoveToEx(erase_dc, g_splitter_x, 95, nullptr);
        LineTo(erase_dc, client_rc.right, 95);
        SelectObject(erase_dc, old_acc);
        DeleteObject(accent);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC: {
        HWND ctrl_wnd = (HWND)lparam;
        if (ctrl_wnd == g_main.label_from || ctrl_wnd == g_main.label_subject ||
            ctrl_wnd == g_main.label_date) {
            HDC ctrl_dc = (HDC)wparam;
            SetTextColor(ctrl_dc, RGB(240, 245, 255));
            SetBkColor(ctrl_dc, RGB(30, 38, 54));
            // Return static brush matching header background
            static HBRUSH header_static_br = CreateSolidBrush(RGB(30, 38, 54));
            return (LRESULT)header_static_br;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    case WM_NOTIFY: {
        NMHDR* notify_header = (NMHDR*)lparam;

        if (notify_header->idFrom == IDC_ATTACH_LIST && notify_header->code == NM_DBLCLK) {
            int selected_index = ListView_GetNextItem(g_main.attach_list, -1, LVNI_SELECTED);
            if (selected_index >= 0 && selected_index < (int)g_app.current_attachments.size())
                save_and_open_attachment(g_app.current_attachments[selected_index]);
            return 0;
        }

        // Bold font for unread messages and alternating row colors via custom draw
        if (notify_header->idFrom == IDC_MAIL_LIST && notify_header->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW* custom_draw = (NMLVCUSTOMDRAW*)lparam;
            if (custom_draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (custom_draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int draw_row = (int)custom_draw->nmcd.dwItemSpec;
                bool is_selected = (custom_draw->nmcd.uItemState & CDIS_SELECTED) != 0;
                // Alternating row background
                if (!is_selected) {
                    custom_draw->clrTextBk = (draw_row % 2 == 0)
                        ? RGB(255, 255, 255)   // white
                        : RGB(248, 249, 251);  // very light blue-gray
                }
                // Bold for unread
                if (g_bold_font && draw_row >= 0 && draw_row < (int)g_app.current_headers.size()
                        && g_app.current_headers[draw_row].is_unread) {
                    SelectObject(custom_draw->nmcd.hdc, g_bold_font);
                }
                return CDRF_NEWFONT;
            }
            return CDRF_DODEFAULT;
        }

        // Clickable hyperlinks in the body RichEdit
        if (notify_header->idFrom == IDC_BODY_EDIT && notify_header->code == EN_LINK) {
            ENLINK* link_notify = (ENLINK*)lparam;
            if (link_notify->msg == WM_LBUTTONUP) {
                // Extract the URL text from the RichEdit at the notified character range
                LONG url_length = link_notify->chrg.cpMax - link_notify->chrg.cpMin;
                if (url_length > 0 && url_length < 2048) {
                    std::wstring url(url_length + 1, L'\0');
                    TEXTRANGEW text_range;
                    text_range.chrg    = link_notify->chrg;
                    text_range.lpstrText = url.data();
                    SendMessageW(g_main.edit_body, EM_GETTEXTRANGE, 0, (LPARAM)&text_range);
                    url.resize(url_length);
                    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            return 1;
        }

        if (notify_header->idFrom == IDC_FOLDER_TREE && notify_header->code == TVN_SELCHANGEDW) {
            NMTREEVIEWW* tree_notify = (NMTREEVIEWW*)lparam;
            TVITEMW folder_item = {};
            folder_item.mask  = TVIF_PARAM;
            folder_item.hItem = tree_notify->itemNew.hItem;
            TreeView_GetItem(g_main.tree_view, &folder_item);
            LPARAM lp = folder_item.lParam;
            if (lp >= 0 && (size_t)lp < g_folder_paths.size()) {
                g_prefetch_stop = true;
                load_folder(window, g_folder_paths[(size_t)lp]);
            }
        }

        if (notify_header->idFrom == IDC_MAIL_LIST && notify_header->code == LVN_COLUMNCLICK) {
            NMLISTVIEW* list_notify = (NMLISTVIEW*)lparam;
            int clicked_column = list_notify->iSubItem;
            if (clicked_column == g_sort_column) {
                g_sort_ascending = !g_sort_ascending; // toggle direction
            } else {
                g_sort_column    = clicked_column;
                g_sort_ascending = true;
            }
            sort_and_repopulate(g_sort_column, g_sort_ascending);
        }

        if (notify_header->idFrom == IDC_MAIL_LIST && notify_header->code == LVN_ITEMCHANGED) {
            NMLISTVIEW* list_notify = (NMLISTVIEW*)lparam;
            if ((list_notify->uNewState & LVIS_SELECTED) && list_notify->iItem >= 0) {
                load_message(window, list_notify->iItem);
            }
            // Update status bar with selected count for multi-select feedback
            int sel_count = ListView_GetSelectedCount(g_main.list_view);
            if (sel_count > 1) {
                std::wstring status_text = std::to_wstring(sel_count) + L" messages selected";
                post_status(window, status_text);
            }
        }

        if (notify_header->hwndFrom == g_main.list_view && notify_header->code == LVN_BEGINDRAG) {
            // Collect selected UIDs for the drag operation
            g_drag_uids.clear();
            int drag_index = -1;
            while ((drag_index = ListView_GetNextItem(g_main.list_view, drag_index, LVNI_SELECTED)) != -1) {
                if (drag_index < (int)g_app.current_headers.size())
                    g_drag_uids.push_back(g_app.current_headers[drag_index].uid);
            }
            if (!g_drag_uids.empty()) {
                g_drag_active = true;
                g_drag_drop_target = nullptr;
                if (!g_cursor_move) g_cursor_move = LoadCursor(nullptr, IDC_HAND);
                if (!g_cursor_no)   g_cursor_no   = LoadCursor(nullptr, IDC_NO);

                // Build ghost drag image from the first selected item
                int first_selected = ListView_GetNextItem(g_main.list_view, -1, LVNI_SELECTED);
                if (first_selected >= 0) {
                    POINT hot_pt = {};  // hotspot relative to item top-left
                    HIMAGELIST single_il = ListView_CreateDragImage(
                        g_main.list_view, first_selected, &hot_pt);
                    if (single_il) {
                        // If multiple items selected, annotate with a count badge
                        int sel_count = (int)g_drag_uids.size();
                        if (sel_count > 1) {
                            // Overlay count text on the image
                            int img_cx = 0, img_cy = 0;
                            ImageList_GetIconSize(single_il, &img_cx, &img_cy);
                            HDC screen_dc = GetDC(nullptr);
                            HDC mem_dc = CreateCompatibleDC(screen_dc);
                            HBITMAP bmp = CreateCompatibleBitmap(screen_dc, img_cx, img_cy);
                            HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, bmp);
                            ImageList_Draw(single_il, 0, mem_dc, 0, 0, ILD_NORMAL);
                            // Draw count badge
                            HBRUSH badge_brush = CreateSolidBrush(RGB(21, 101, 192));
                            RECT badge_rect = {img_cx - 16, 0, img_cx, 16};
                            FillRect(mem_dc, &badge_rect, badge_brush);
                            DeleteObject(badge_brush);
                            SetTextColor(mem_dc, RGB(255, 255, 255));
                            SetBkMode(mem_dc, TRANSPARENT);
                            std::wstring count_str = std::to_wstring(sel_count);
                            DrawTextW(mem_dc, count_str.c_str(), -1, &badge_rect,
                                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                            SelectObject(mem_dc, old_bmp);
                            DeleteDC(mem_dc);
                            ReleaseDC(nullptr, screen_dc);
                            // Recreate image list with the annotated bitmap
                            ImageList_Destroy(single_il);
                            single_il = ImageList_Create(img_cx, img_cy, ILC_COLOR32, 1, 0);
                            ImageList_Add(single_il, bmp, nullptr);
                            DeleteObject(bmp);
                        }
                        // Start the system drag
                        if (g_drag_image_list) { ImageList_EndDrag(); ImageList_Destroy(g_drag_image_list); }
                        g_drag_image_list = single_il;
                        POINT cursor_screen;
                        GetCursorPos(&cursor_screen);
                        ImageList_BeginDrag(g_drag_image_list, 0, hot_pt.x, hot_pt.y);
                        ImageList_DragEnter(GetDesktopWindow(), cursor_screen.x, cursor_screen.y);
                    }
                }

                SetCapture(window);
            }
            return 0;
        }

        return 0;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lparam;
        // Determine button color and label
        COLORREF bg_color = RGB(84, 110, 122);
        const wchar_t* label = L"";
        switch (dis->CtlID) {
            case IDC_BTN_REPLY:     bg_color = RGB(46, 125, 50);   label = L"Reply";     break;
            case IDC_BTN_REPLY_ALL: bg_color = RGB(46, 125, 50);   label = L"Reply All"; break;
            case IDC_BTN_FORWARD:   bg_color = RGB(46, 125, 50);   label = L"Forward";   break;
            case IDC_BTN_COMPOSE:   bg_color = RGB(21, 101, 192);  label = L"Compose";   break;
            case IDC_BTN_REFRESH:   bg_color = RGB(84, 110, 122);  label = L"Refresh";   break;
            case IDC_BTN_DELETE:    bg_color = RGB(198, 40, 40);   label = L"Delete";    break;
            case IDC_BTN_SETTINGS:  bg_color = RGB(80, 80, 100);   label = L"Accounts";  break;
            default: return DefWindowProcW(window, message, wparam, lparam);
        }
        // Darken on press
        if (dis->itemState & ODS_SELECTED) {
            int red_val   = GetRValue(bg_color) * 3 / 4;
            int green_val = GetGValue(bg_color) * 3 / 4;
            int blue_val  = GetBValue(bg_color) * 3 / 4;
            bg_color = RGB(red_val, green_val, blue_val);
        }
        HDC draw_dc = dis->hDC;
        RECT draw_rect = dis->rcItem;
        // Fill background
        HBRUSH btn_brush = CreateSolidBrush(bg_color);
        FillRect(draw_dc, &draw_rect, btn_brush);
        DeleteObject(btn_brush);
        // Draw text
        SetTextColor(draw_dc, RGB(255, 255, 255));
        SetBkMode(draw_dc, TRANSPARENT);
        DrawTextW(draw_dc, label, -1, &draw_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        // Draw focus rectangle if needed
        if (dis->itemState & ODS_FOCUS)
            DrawFocusRect(draw_dc, &draw_rect);
        return TRUE;
    }

    case WM_COMMAND: {
        int control_id = LOWORD(wparam);
        if (control_id == IDC_ATTACH_LIST && HIWORD(wparam) == LBN_DBLCLK) {
            int selected_index = ListView_GetNextItem(g_main.attach_list, -1, LVNI_SELECTED);
            if (selected_index >= 0 && selected_index < (int)g_app.current_attachments.size()) {
                save_and_open_attachment(g_app.current_attachments[selected_index]);
            }
        } else if (control_id == IDC_BTN_COMPOSE) {
            open_compose_window(window);
        } else if (control_id == IDC_BTN_REPLY) {
            if (!g_app.current_uid.empty()) {
                std::wstring reply_to    = utf8_to_wide(g_app.current_from);
                std::wstring reply_subj  = L"Re: " + utf8_to_wide(g_app.current_subject);
                std::wstring reply_body  = L"\r\n\r\n--- Original Message ---\r\n" + utf8_to_wide(g_app.current_body);
                open_compose_window(window, reply_to, reply_subj, reply_body);
            }
        } else if (control_id == IDC_BTN_REPLY_ALL) {
            if (!g_app.current_uid.empty()) {
                std::wstring reply_to   = utf8_to_wide(g_app.current_from);
                std::wstring reply_subj = L"Re: " + utf8_to_wide(g_app.current_subject);
                std::wstring reply_body = L"\r\n\r\n--- Original Message ---\r\n" + utf8_to_wide(g_app.current_body);
                open_compose_window(window, reply_to, reply_subj, reply_body);
            }
        } else if (control_id == IDC_BTN_FORWARD) {
            if (!g_app.current_uid.empty()) {
                std::wstring fwd_subj  = L"Fwd: " + utf8_to_wide(g_app.current_subject);
                std::wstring fwd_body  = L"\r\n\r\n--- Forwarded Message ---\r\n" + utf8_to_wide(g_app.current_body);
                open_compose_window(window, L"", fwd_subj, fwd_body);
            }
        } else if (control_id == IDC_BTN_DELETE) {
            delete_selected_messages();
        } else if (control_id == IDC_BTN_REFRESH) {
            if (g_app.imap_connected) {
                HTREEITEM selected_item = TreeView_GetSelection(g_main.tree_view);
                if (selected_item) {
                    TVITEMW folder_item = {};
                    folder_item.mask  = TVIF_PARAM;
                    folder_item.hItem = selected_item;
                    TreeView_GetItem(g_main.tree_view, &folder_item);
                    LPARAM lp = folder_item.lParam;
                    if (lp >= 0 && (size_t)lp < g_folder_paths.size()) {
                        ++g_load_generation; // force reload even if same folder
                        load_folder(window, g_folder_paths[(size_t)lp]);
                    }
                }
            } else {
                start_connect(window);
            }
        } else if (control_id == IDC_BTN_SETTINGS) {
            // Show popup menu: list of accounts + Add Account + Manage Accounts
            HMENU popup = CreatePopupMenu();

            // Existing accounts
            for (int acct_idx = 0; acct_idx < (int)g_accounts.size(); ++acct_idx) {
                const auto& acct = g_accounts[acct_idx];
                std::wstring label = utf8_to_wide(acct.email);
                if (label.empty()) label = utf8_to_wide(acct.display_name);
                UINT flags = MF_STRING;
                if (acct_idx == g_active_account_index) flags |= MF_CHECKED;
                AppendMenuW(popup, flags, 6000 + acct_idx, label.c_str());
            }
            if (!g_accounts.empty()) AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(popup, MF_STRING, 6900, L"+ Add Account");
            AppendMenuW(popup, MF_STRING, 6901, L"⚙ Manage Accounts");

            // Show below the button
            RECT btn_rc = {};
            GetWindowRect(g_main.btn_settings, &btn_rc);
            int chosen = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_LEFTBUTTON,
                                        btn_rc.left, btn_rc.bottom, 0, window, nullptr);
            DestroyMenu(popup);

            if (chosen >= 6000 && chosen < 6900) {
                // Switch to this account
                int idx = chosen - 6000;
                if (idx < (int)g_accounts.size() && idx != g_active_account_index) {
                    g_active_account_index = idx;
                    g_account           = g_accounts[idx];
                    g_app.email_address = utf8_to_wide(g_account.email);
                    g_app.app_password  = utf8_to_wide(g_account.password);
                    g_app.imap_connected = false;
                    g_prefetch_stop = true;
                    if (g_app.imap_client)     g_app.imap_client->disconnect();
                    if (g_app.prefetch_client) g_app.prefetch_client->disconnect();
                    start_connect(window);
                }
            } else if (chosen == 6900 || chosen == 6901) {
                // 6900 = Add new (edit_index=-1), 6901 = Manage active account
                int mgr_idx = (chosen == 6901) ? g_active_account_index : -1;
                if (show_account_settings(window, mgr_idx)) {
                    g_account           = g_settings_state.config;
                    g_app.email_address = utf8_to_wide(g_account.email);
                    g_app.app_password  = utf8_to_wide(g_account.password);
                    g_app.imap_connected = false;
                    g_prefetch_stop = true;
                    if (g_app.imap_client)     g_app.imap_client->disconnect();
                    if (g_app.prefetch_client) g_app.prefetch_client->disconnect();
                    start_connect(window);
                }
            }
        }
        return 0;
    }

    case WM_IMAP_FOLDERS: {
        auto* folder_vec = (std::vector<std::string>*)wparam;
        populate_folder_tree(*folder_vec);
        delete folder_vec;
        return 0;
    }

    case WM_IMAP_HEADERS: {
        auto* header_vec = (std::vector<MailHeader>*)wparam;
        g_app.current_headers = *header_vec;
        populate_mail_list(*header_vec);
        delete header_vec;
        // Persist headers to disk for instant display on next folder open
        if (g_app.imap_client && !g_app.current_folder.empty() &&
            !g_app.current_headers.empty()) {
            std::string folder_snapshot = g_app.current_folder;
            std::string cache_key       = g_app.imap_client->get_email_address();
            std::vector<MailHeader> headers_copy = g_app.current_headers;
            std::thread([folder_snapshot, cache_key,
                         headers_copy = std::move(headers_copy)]() {
                cache_write_headers(folder_snapshot, cache_key, headers_copy);
            }).detach();
        }
        // Start background prefetch for all uncached messages
        if (g_app.imap_client && g_app.imap_connected)
            start_background_prefetch(g_app.current_folder, g_app.current_headers, window);
        return 0;
    }

    case WM_IMAP_HEADERS_APPEND: {
        auto* header_vec = (std::vector<MailHeader>*)wparam;
        for (auto& header : *header_vec) g_app.current_headers.push_back(header);
        append_to_mail_list(*header_vec);
        delete header_vec;
        return 0;
    }

    case WM_IMAP_BODY: {
        auto* result = (BodyResult*)wparam;

        // Display body: prefer raw HTML for faithful rendering; fall back to plain text
        if (g_browser_host) {
            if (!result->html_raw.empty())
                g_browser_host->navigate_html(result->html_raw);
            else
                g_browser_host->navigate_text(result->text);
        }

        // Update attachment list
        g_app.current_attachments = std::move(result->attachments);
        ListView_DeleteAllItems(g_main.attach_list);
        // Get the system large image list (48px icons) and associate once
        {
            SHFILEINFO sfi_tmp = {};
            HIMAGELIST sys_il = (HIMAGELIST)SHGetFileInfo(L"x.bin", FILE_ATTRIBUTE_NORMAL,
                &sfi_tmp, sizeof(sfi_tmp),
                SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES | SHGFI_LARGEICON);
            if (sys_il) ListView_SetImageList(g_main.attach_list, sys_il, LVSIL_NORMAL);
        }
        int item_index = 0;
        for (const auto& att : g_app.current_attachments) {
            // Get shell icon index for this file extension
            std::wstring wide_name = utf8_to_wide(att.filename);
            SHFILEINFO sfi = {};
            SHGetFileInfo(wide_name.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES | SHGFI_LARGEICON);

            // Build display label: filename + size on second line
            // Use declared_size from BODYSTRUCTURE when raw_bytes not yet downloaded
            size_t byte_count = att.raw_bytes.empty() ? att.declared_size : att.raw_bytes.size();
            size_t kb = byte_count / 1024;
            wchar_t size_buf[32];
            if (kb > 0) swprintf(size_buf, 32, L" (%zu KB)", kb);
            else         swprintf(size_buf, 32, L" (%zu B)",  byte_count);
            std::wstring label = wide_name + L"\n" + size_buf;

            LVITEMW lvi = {};
            lvi.mask    = LVIF_TEXT | LVIF_IMAGE;
            lvi.iItem   = item_index++;
            lvi.iImage  = sfi.iIcon;
            lvi.pszText = const_cast<wchar_t*>(label.c_str());
            ListView_InsertItem(g_main.attach_list, &lvi);
        }
        // Show/hide attachment panel
        bool has_attachments = !g_app.current_attachments.empty();
        ShowWindow(g_main.label_attachments, has_attachments ? SW_SHOW : SW_HIDE);
        ShowWindow(g_main.attach_list,       has_attachments ? SW_SHOW : SW_HIDE);
        // Re-trigger WM_SIZE to reflow the body height
        RECT client_rect;
        GetClientRect(window, &client_rect);
        SendMessageW(window, WM_SIZE, 0,
                     MAKELPARAM(client_rect.right, client_rect.bottom));

        delete result;
        return 0;
    }

    case WM_STATUS_TEXT: {
        auto* status_text = (std::wstring*)lparam;
        set_status(*status_text);
        delete status_text;
        return 0;
    }

    case WM_SMTP_DONE: {
        auto* status_text = (std::wstring*)lparam;
        set_status(*status_text);
        delete status_text;
        if (wparam == 0) {
            MessageBoxW(window, L"Message sent successfully.", L"Sent", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(window, L"Failed to send message. Check status bar.", L"Error", MB_OK | MB_ICONERROR);
        }
        return 0;
    }

    case WM_IMAP_DELETE_DONE: {
        int deleted_row = (int)wparam;
        if (deleted_row >= 0 && deleted_row < (int)g_app.current_headers.size()) {
            g_app.current_headers.erase(g_app.current_headers.begin() + deleted_row);
            ListView_DeleteItem(g_main.list_view, deleted_row);
            // Fix lParam indices for rows that shifted up
            for (int idx = deleted_row; idx < ListView_GetItemCount(g_main.list_view); ++idx) {
                LVITEMW item = {};
                item.mask   = LVIF_PARAM;
                item.iItem  = idx;
                item.lParam = (LPARAM)idx;
                ListView_SetItem(g_main.list_view, &item);
            }
            g_app.current_uid.clear();
            if (g_browser_host) g_browser_host->navigate_text("");
            SetWindowTextW(g_main.label_from,    L"");
            SetWindowTextW(g_main.label_subject, L"");
            SetWindowTextW(g_main.label_date,    L"");
            set_status(L"Message deleted.");
        } else {
            set_status(L"Delete failed.");
        }
        return 0;
    }

    case WM_IMAP_READ_UPDATE: {
        int read_row = (int)wparam;
        if (read_row >= 0 && read_row < (int)g_app.current_headers.size()) {
            g_app.current_headers[read_row].is_unread = false;
            RECT row_rect;
            ListView_GetItemRect(g_main.list_view, read_row, &row_rect, LVIR_BOUNDS);
            InvalidateRect(g_main.list_view, &row_rect, TRUE);
        }
        return 0;
    }

    case WM_DELETE_PROGRESS: {
        int percent_done = (int)wparam;
        SendMessageW(g_main.progress_bar, PBM_SETPOS, (WPARAM)percent_done, 0);
        if (percent_done >= 100) {
            Sleep(300);
            ShowWindow(g_main.progress_bar, SW_HIDE);
        }
        return 0;
    }

    case WM_SETCURSOR: {
        if ((HWND)wparam == window) {
            POINT cursor_pt;
            GetCursorPos(&cursor_pt);
            ScreenToClient(window, &cursor_pt);
            if (cursor_pt.x >= g_splitter_x - 4 && cursor_pt.x <= g_splitter_x + 4) {
                SetCursor(g_cursor_sizewe);
                return TRUE;
            }
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int click_x = GET_X_LPARAM(lparam);
        if (click_x >= g_splitter_x - 4 && click_x <= g_splitter_x + 4) {
            g_splitter_drag = true;
            SetCapture(window);
            SetCursor(g_cursor_sizewe);
        }
        break;
    }

    case WM_MOUSEMOVE: {
        if (g_splitter_drag) {
            int mouse_x = GET_X_LPARAM(lparam);
            g_splitter_x = mouse_x;
            SetCursor(g_cursor_sizewe);
            // Trigger WM_SIZE to reflow layout
            RECT client_rect;
            GetClientRect(window, &client_rect);
            SendMessageW(window, WM_SIZE, SIZE_RESTORED,
                         MAKELPARAM(client_rect.right, client_rect.bottom));
        }
        if (g_drag_active) {
            POINT cursor_pt;
            GetCursorPos(&cursor_pt);

            // Move the ghost drag image on the desktop (screen coords)
            if (g_drag_image_list)
                ImageList_DragMove(cursor_pt.x, cursor_pt.y);

            // Hit-test the tree view to highlight drop target
            POINT tree_pt = cursor_pt;
            ScreenToClient(g_main.tree_view, &tree_pt);
            TVHITTESTINFO ht = {};
            ht.pt = tree_pt;
            // Hide drag image while updating tree (avoids flicker)
            if (g_drag_image_list) ImageList_DragShowNolock(FALSE);
            HTREEITEM hit = TreeView_HitTest(g_main.tree_view, &ht);
            if (hit && (ht.flags & (TVHT_ONITEM | TVHT_ONITEMICON | TVHT_ONITEMLABEL))) {
                if (hit != g_drag_drop_target) {
                    TreeView_SelectDropTarget(g_main.tree_view, hit);
                    g_drag_drop_target = hit;
                }
                SetCursor(g_cursor_move ? g_cursor_move : LoadCursor(nullptr, IDC_HAND));
            } else {
                TreeView_SelectDropTarget(g_main.tree_view, nullptr);
                g_drag_drop_target = nullptr;
                SetCursor(g_cursor_no ? g_cursor_no : LoadCursor(nullptr, IDC_NO));
            }
            if (g_drag_image_list) ImageList_DragShowNolock(TRUE);
            return 0;
        }
        break;
    }

    case WM_LBUTTONUP: {
        if (g_splitter_drag) {
            g_splitter_drag = false;
            ReleaseCapture();
        }
        if (g_drag_active) {
            g_drag_active = false;
            // End ghost image before any UI updates
            if (g_drag_image_list) {
                ImageList_EndDrag();
                ImageList_Destroy(g_drag_image_list);
                g_drag_image_list = nullptr;
            }
            ReleaseCapture();
            TreeView_SelectDropTarget(g_main.tree_view, nullptr);

            if (g_drag_drop_target && !g_drag_uids.empty()) {
                // Get the IMAP folder path from the tree item lParam
                TVITEMW tvi = {};
                tvi.hItem = g_drag_drop_target;
                tvi.mask  = TVIF_PARAM;
                TreeView_GetItem(g_main.tree_view, &tvi);
                LPARAM folder_param = tvi.lParam;
                std::string dest_folder;
                if (folder_param >= 0 && (size_t)folder_param < g_folder_paths.size())
                    dest_folder = g_folder_paths[(size_t)folder_param];

                if (!dest_folder.empty() && dest_folder != g_app.current_folder &&
                    g_app.imap_client && g_app.imap_connected) {
                    std::string source_folder = g_app.current_folder;
                    std::vector<std::string> uids_to_move = g_drag_uids;
                    std::thread([source_folder, dest_folder, uids_to_move]() {
                        g_app.imap_client->move_messages(source_folder, uids_to_move, dest_folder);
                        PostMessageW(GetAncestor(g_main.list_view, GA_ROOT),
                                     WM_COMMAND, IDC_BTN_REFRESH, 0);
                    }).detach();

                    post_status(window, L"Moving messages...");
                }
            }
            g_drag_uids.clear();
            g_drag_drop_target = nullptr;
            return 0;
        }
        break;
    }

    case WM_CONTEXTMENU: {
        // Only handle right-clicks on the folder tree
        if ((HWND)wparam != g_main.tree_view) break;
        if (!g_app.imap_client || !g_app.imap_connected) break;

        // Hit-test to find the clicked tree item
        POINT screen_pt  = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        POINT client_pt  = screen_pt;
        ScreenToClient(g_main.tree_view, &client_pt);
        TVHITTESTINFO hit_test = {};
        hit_test.pt = client_pt;
        HTREEITEM hit_item = TreeView_HitTest(g_main.tree_view, &hit_test);
        if (hit_item) TreeView_SelectItem(g_main.tree_view, hit_item);

        // Read the display name of the hit item (used as the folder path for top-level folders)
        std::wstring selected_folder_wide;
        std::string  selected_folder_path;
        if (hit_item) {
            wchar_t text_buf[512] = {};
            TVITEMW tree_item = {};
            tree_item.hItem      = hit_item;
            tree_item.mask       = TVIF_TEXT | TVIF_PARAM;
            tree_item.pszText    = text_buf;
            tree_item.cchTextMax = 512;
            TreeView_GetItem(g_main.tree_view, &tree_item);
            selected_folder_wide = text_buf;
            // Resolve full IMAP path via lParam index into g_folder_paths.
            // If lParam==-1 (intermediate container node), walk up the tree
            // collecting segment names until we find a node with a known path.
            LPARAM folder_param = tree_item.lParam;
            if (folder_param >= 0 && (size_t)folder_param < g_folder_paths.size()) {
                selected_folder_path = g_folder_paths[(size_t)folder_param];
            } else {
                // Build path by walking up: collect display names
                std::string built_path;
                HTREEITEM walk = hit_item;
                while (walk) {
                    wchar_t seg_buf[512] = {};
                    TVITEMW seg_item = {};
                    seg_item.hItem = walk; seg_item.mask = TVIF_TEXT | TVIF_PARAM;
                    seg_item.pszText = seg_buf; seg_item.cchTextMax = 512;
                    TreeView_GetItem(g_main.tree_view, &seg_item);
                    // If this ancestor has a known full path, prepend it
                    if (seg_item.lParam >= 0 &&
                        (size_t)seg_item.lParam < g_folder_paths.size()) {
                        std::string base = g_folder_paths[(size_t)seg_item.lParam];
                        selected_folder_path = built_path.empty()
                            ? base : base + "." + built_path;
                        break;
                    }
                    std::string seg = wide_to_utf8(seg_buf);
                    built_path = built_path.empty() ? seg : seg + "." + built_path;
                    walk = TreeView_GetParent(g_main.tree_view, walk);
                }
                if (selected_folder_path.empty())
                    selected_folder_path = built_path;
            }
        }

        // Build context menu
        HMENU context_menu = CreatePopupMenu();
        AppendMenuW(context_menu, MF_STRING, IDM_NEW_FOLDER,
                    selected_folder_path.empty()
                        ? L"New top-level folder..."
                        : (L"New subfolder inside \"" + selected_folder_wide + L"\"...").c_str());
        if (!selected_folder_path.empty()) {
            AppendMenuW(context_menu, MF_SEPARATOR, 0, nullptr);
            bool is_inbox = (to_lower(selected_folder_path) == "inbox");
            AppendMenuW(context_menu,
                        MF_STRING | (is_inbox ? MF_GRAYED : 0),
                        IDM_RENAME_FOLDER, L"Rename...");
            AppendMenuW(context_menu,
                        MF_STRING | (is_inbox ? MF_GRAYED : 0),
                        IDM_DELETE_FOLDER, L"Delete folder");
        }

        int chosen_command = TrackPopupMenu(context_menu,
                                            TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                            screen_pt.x, screen_pt.y,
                                            0, window, nullptr);
        DestroyMenu(context_menu);

        if (chosen_command == IDM_NEW_FOLDER) {
            std::wstring folder_label = selected_folder_path.empty()
                ? L"New top-level folder name:"
                : (L"New subfolder inside \"" + selected_folder_wide + L"\":");
            std::wstring new_folder_name = show_input_dialog(
                window, L"Create Folder", folder_label, L"New Folder");
            if (!new_folder_name.empty()) {
                std::string full_path;
                if (selected_folder_path.empty()) {
                    full_path = wide_to_utf8(new_folder_name);
                } else {
                    full_path = selected_folder_path + "." + wide_to_utf8(new_folder_name);
                }
                std::thread([full_path, window]() {
                    bool created = g_app.imap_client &&
                                   g_app.imap_client->create_mailbox(full_path);
                    if (created) {
                        post_status(window,
                            utf8_to_wide("Folder \"" + full_path + "\" created."));
                        // Refresh the folder list by reloading folders from server
                        auto folders = g_app.imap_client->list_folders();
                        auto* folder_vec = new std::vector<std::string>(std::move(folders));
                        PostMessageW(window, WM_IMAP_FOLDERS, (WPARAM)folder_vec, 0);
                    } else {
                        post_status(window,
                            utf8_to_wide("Failed to create folder: " + full_path));
                    }
                }).detach();
            }
        } else if (chosen_command == IDM_RENAME_FOLDER && !selected_folder_path.empty()) {
            std::wstring new_name = show_input_dialog(
                window, L"Rename Folder",
                L"New name for \"" + selected_folder_wide + L"\":",
                selected_folder_wide);
            if (!new_name.empty() && new_name != selected_folder_wide) {
                std::string old_path = selected_folder_path;
                std::string new_path = wide_to_utf8(new_name);
                std::thread([old_path, new_path, window]() {
                    bool renamed = g_app.imap_client &&
                                   g_app.imap_client->rename_mailbox(old_path, new_path);
                    if (renamed) {
                        post_status(window,
                            utf8_to_wide("Folder renamed to \"" + new_path + "\"."));
                    } else {
                        post_status(window, L"Rename failed.");
                    }
                    // Refresh folder list regardless of outcome
                    auto folders = g_app.imap_client->list_folders();
                    auto* folder_vec = new std::vector<std::string>(std::move(folders));
                    PostMessageW(window, WM_IMAP_FOLDERS, (WPARAM)folder_vec, 0);
                }).detach();
            }
        } else if (chosen_command == IDM_DELETE_FOLDER && !selected_folder_path.empty()) {
            std::wstring confirm_text =
                L"Delete folder \"" + selected_folder_wide +
                L"\"?\nThis cannot be undone.";
            if (MessageBoxW(window, confirm_text.c_str(), L"Confirm Delete",
                            MB_YESNO | MB_ICONWARNING) == IDYES) {
                std::string delete_path = selected_folder_path;
                std::thread([delete_path, window]() {
                    bool deleted = g_app.imap_client &&
                                   g_app.imap_client->delete_mailbox(delete_path);
                    if (deleted) {
                        post_status(window,
                            utf8_to_wide("Folder \"" + delete_path + "\" deleted."));
                    } else {
                        post_status(window, L"Delete failed.");
                    }
                    // Refresh folder list regardless
                    auto folders = g_app.imap_client->list_folders();
                    auto* folder_vec = new std::vector<std::string>(std::move(folders));
                    PostMessageW(window, WM_IMAP_FOLDERS, (WPARAM)folder_vec, 0);
                }).detach();
            }
        }
        return 0;
    }

    case WM_DESTROY:
        g_prefetch_stop = true;
        if (g_browser_host) { g_browser_host->destroy(); g_browser_host = nullptr; }
        if (g_app.imap_client) {
            g_app.imap_client->disconnect();
            g_app.imap_client.reset();
        }
        if (g_body_font) { DeleteObject(g_body_font); g_body_font = nullptr; }
        if (g_bold_font) { DeleteObject(g_bold_font); g_bold_font = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// Forward declaration (defined below)
static HICON create_app_icon(int size);

// ─── Splash screen ───────────────────────────────────────────────────────────
static LRESULT CALLBACK splash_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client_rect;
        GetClientRect(hwnd, &client_rect);
        int width  = client_rect.right;
        int height = client_rect.bottom;

        // Gradient background: dark blue top → medium blue bottom
        for (int row = 0; row < height; ++row) {
            float t = (float)row / (float)height;
            int red   = (int)(10  + t * 20);
            int green = (int)(50  + t * 60);
            int blue  = (int)(160 + t * 60);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(red, green, blue));
            HPEN old_pen = (HPEN)SelectObject(dc, pen);
            MoveToEx(dc, 0, row, nullptr);
            LineTo(dc, width, row);
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }

        // Envelope icon large
        HICON env_icon = create_app_icon(64);
        if (env_icon) {
            DrawIconEx(dc, width / 2 - 32, height / 2 - 70, env_icon, 64, 64,
                       0, nullptr, DI_NORMAL);
            DestroyIcon(env_icon);
        }

        // App name
        HFONT title_font = CreateFontW(-36, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT old_font = (HFONT)SelectObject(dc, title_font);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        RECT title_rect = {0, height / 2 + 10, width, height / 2 + 60};
        DrawTextW(dc, L"FastMail", -1, &title_rect, DT_CENTER | DT_SINGLELINE);

        // Subtitle
        DeleteObject(SelectObject(dc, CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI")));
        SetTextColor(dc, RGB(180, 210, 255));
        RECT sub_rect = {0, height / 2 + 62, width, height / 2 + 90};
        DrawTextW(dc, L"Native Windows Client", -1, &sub_rect, DT_CENTER | DT_SINGLELINE);

        SelectObject(dc, old_font);
        DeleteObject(title_font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_TIMER) {
        KillTimer(hwnd, 1);
        DestroyWindow(hwnd);
        g_splash_window = nullptr;
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        KillTimer(hwnd, 1);
        DestroyWindow(hwnd);
        g_splash_window = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void show_splash_screen(HINSTANCE instance) {
    WNDCLASSEXW sc = {};
    sc.cbSize        = sizeof(sc);
    sc.lpfnWndProc   = splash_wnd_proc;
    sc.hInstance     = instance;
    sc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    sc.lpszClassName = L"FastMailSplash";
    RegisterClassExW(&sc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = 420, wh = 260;
    g_splash_window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"FastMailSplash", L"",
        WS_POPUP | WS_VISIBLE,
        (sw - ww) / 2, (sh - wh) / 2, ww, wh,
        nullptr, nullptr, instance, nullptr);
    if (g_splash_window) {
        SetTimer(g_splash_window, 1, 5000, nullptr);
        UpdateWindow(g_splash_window);
    }
}

// ─── App icon (programmatic envelope) ────────────────────────────────────────
static HICON create_app_icon(int size) {
    BITMAPINFOHEADER bmi = {};
    bmi.biSize        = sizeof(bmi);
    bmi.biWidth       = size;
    bmi.biHeight      = -size; // top-down
    bmi.biPlanes      = 1;
    bmi.biBitCount    = 32;
    bmi.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen_dc = GetDC(nullptr);
    HBITMAP color_bmp = CreateDIBSection(screen_dc, (BITMAPINFO*)&bmi,
                                          DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen_dc);
    if (!color_bmp) return LoadIcon(nullptr, IDI_APPLICATION);

    HDC mem_dc  = CreateCompatibleDC(nullptr);
    HBITMAP old = (HBITMAP)SelectObject(mem_dc, color_bmp);

    // Blue rounded background
    HBRUSH bg = CreateSolidBrush(RGB(28, 100, 200));
    RECT   rc = {0, 0, size, size};
    FillRect(mem_dc, &rc, bg);
    DeleteObject(bg);

    // White envelope body
    int m  = size / 6;
    int ey = size * 5 / 12;
    int eb = size * 4 / 6;
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    RECT env = {m, ey, size - m, eb};
    FillRect(mem_dc, &env, white);
    DeleteObject(white);

    // Light-blue envelope flap (triangle)
    POINT tri[3] = {
        {m,        ey},
        {size / 2, ey + (eb - ey) / 2},
        {size - m, ey}
    };
    HBRUSH flap = CreateSolidBrush(RGB(180, 210, 255));
    HRGN   rgn  = CreatePolygonRgn(tri, 3, WINDING);
    FillRgn(mem_dc, rgn, flap);
    DeleteObject(rgn);
    DeleteObject(flap);

    SelectObject(mem_dc, old);
    DeleteDC(mem_dc);

    HBITMAP mask_bmp = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon    = TRUE;
    ii.hbmColor = color_bmp;
    ii.hbmMask  = mask_bmp;
    HICON icon  = CreateIconIndirect(&ii);
    DeleteObject(color_bmp);
    DeleteObject(mask_bmp);
    return icon ? icon : LoadIcon(nullptr, IDI_APPLICATION);
}

// ─── Entry point ─────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_cmd) {
    // Install crash handlers before anything else
    SetUnhandledExceptionFilter(unhandled_exception_filter);
    std::set_terminate([]() {
        debug_log("FATAL: std::terminate() called (unhandled C++ exception in thread)");
        MessageBoxA(nullptr,
                    "FastMail crashed (std::terminate).\nSee %TEMP%\\fastmail_log.txt",
                    "Fatal Error", MB_OK | MB_ICONERROR);
        ExitProcess(1);
    });

    // Clear log from previous run
    {
        char temp_path[MAX_PATH];
        GetTempPathA(MAX_PATH, temp_path);
        std::string log_path = std::string(temp_path) + "fastmail_log.txt";
        DeleteFileA(log_path.c_str());
    }
    debug_log("FastMail started");

    // Disable IE Local Machine Zone lockdown so that our local HTML preview
    // files can load external images/CSS from https:// URLs without being
    // blocked by cross-zone security. Must be called before OleInitialize.
    {
        typedef HRESULT (WINAPI *PFN_SetFeature)(int, DWORD, BOOL);
        HMODULE urlmon_lib = LoadLibraryA("urlmon.dll");
        if (urlmon_lib) {
            auto set_feature = (PFN_SetFeature)
                GetProcAddress(urlmon_lib, "CoInternetSetFeatureEnabled");
            if (set_feature) {
                set_feature(8 /*FEATURE_LOCALMACHINE_LOCKDOWN*/,   2 /*SET_FEATURE_ON_PROCESS*/, FALSE);
                set_feature(21/*FEATURE_RESTRICT_FILEDOWNLOAD*/,   2, FALSE);
                set_feature(14/*FEATURE_SECURITYBAND*/,            2, FALSE);
            }
        }
    }

    // Init OLE (required for embedded WebBrowser), sockets, common controls
    OleInitialize(nullptr);
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);

    INITCOMMONCONTROLSEX icce = {};
    icce.dwSize = sizeof(icce);
    icce.dwICC  = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icce);

    // Register main window class
    WNDCLASSW wnd_class = {};
    wnd_class.lpfnWndProc   = main_wnd_proc;
    wnd_class.hInstance     = instance;
    wnd_class.lpszClassName = L"FastMailClient";
    wnd_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wnd_class.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wnd_class.hIcon         = create_app_icon(32);
    RegisterClassW(&wnd_class);

    // Show splash screen (5 seconds or click to dismiss)
    show_splash_screen(instance);

    // Load account config (new) or fall back to legacy config
    if (load_account_config(g_account)) {
        g_app.email_address = utf8_to_wide(g_account.email);
        g_app.app_password  = utf8_to_wide(g_account.password);
    } else {
        load_config(g_app.email_address, g_app.app_password);
    }
    // Load all accounts list (multi-account support)
    if (!load_all_accounts(g_accounts) && !g_account.email.empty()) {
        // Seed from single-account config if accounts.cfg does not exist yet
        g_accounts.push_back(g_account);
    }

    // Create main window
    HWND main_hwnd = CreateWindowW(L"FastMailClient", L"FastMail",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 700,
        nullptr, nullptr, instance, nullptr);

    if (!main_hwnd) return 1;

    SendMessageW(main_hwnd, WM_SETICON, ICON_BIG,   (LPARAM)create_app_icon(32));
    SendMessageW(main_hwnd, WM_SETICON, ICON_SMALL,  (LPARAM)create_app_icon(16));

    ShowWindow(main_hwnd, show_cmd);
    UpdateWindow(main_hwnd);

    // Show account settings if no credentials are saved yet
    if (g_app.email_address.empty() || g_app.app_password.empty()) {
        if (show_account_settings(main_hwnd)) {
            g_account           = g_settings_state.config;
            g_app.email_address = utf8_to_wide(g_account.email);
            g_app.app_password  = utf8_to_wide(g_account.password);
        } else {
            // Fall back to legacy login dialog if user cancelled settings
            show_login_dialog(main_hwnd);
        }
    }

    // Start IMAP connection if we have credentials
    if (!g_app.email_address.empty() && !g_app.app_password.empty()) {
        start_connect(main_hwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    OleUninitialize();
    return (int)msg.wParam;
}
