#include <string>
#include <regex>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <vector>
#include <arpa/inet.h>

class ValidationError : public std::runtime_error {
public:
    explicit ValidationError(const std::string& msg,
                             const std::string& code  = "",
                             const std::string& value = "")
        : std::runtime_error(msg), m_code(code), m_value(value)
    {}

    const std::string& code()  const { return m_code; }
    const std::string& value() const { return m_value; }

private:
    std::string m_code;
    std::string m_value;
};

inline bool regex_matches(const std::string& pattern,
                          const std::string& value,
                          std::regex_constants::syntax_option_type flags =
                              std::regex_constants::ECMAScript)
{
    try {
        std::regex re(pattern, flags);
        return std::regex_search(value, re);
    }
    catch (const std::regex_error&) {
        throw std::runtime_error("Failed to compile regex pattern: " + pattern);
    }
}

class RegexValidator {
public:
    RegexValidator(const std::string& regexPattern = "",
                   const std::string& message      = "Enter a valid value.",
                   const std::string& code         = "invalid",
                   bool inverse_match              = false,
                   std::regex_constants::syntax_option_type flags =
                       std::regex_constants::ECMAScript)
        : m_pattern(regexPattern),
          m_message(message),
          m_code(code),
          m_inverse(inverse_match),
          m_flags(flags)
    {
        if (!m_pattern.empty()) {

            std::regex re(m_pattern, m_flags);
        }
    }

    virtual ~RegexValidator() {}

    virtual void operator()(const std::string& value) const {
        bool matched = false;
        if (!m_pattern.empty()) {
            matched = regex_matches(m_pattern, value, m_flags);
        }

        bool invalid = m_inverse ? matched : !matched;
        if (invalid) {
            throw ValidationError(m_message, m_code, value);
        }
    }

protected:
    std::string m_pattern;
    std::string m_message;
    std::string m_code;
    bool m_inverse;
    std::regex_constants::syntax_option_type m_flags;
};

class EmailValidator {
public:
    EmailValidator(const std::string& message = "Enter a valid email address.",
                   const std::string& code    = "invalid",
                   const std::vector<std::string>& allowlist = {"localhost"})
        : m_message(message), m_code(code), m_domain_allowlist(allowlist)
    {

        m_user_re = std::regex(
            R"((^[-!#$%&'*+/=?^_`{}|~0-9A-Z]+(\.[-!#$%&'*+/=?^_`{}|~0-9A-Z]+)*$)|)"
            R"(^"([\001-\010\013\014\016-\037!#-\[\]-\177]|\\[\001-\011\013\014\016-\177])*"$)",
            std::regex_constants::icase);

        m_domain_re = std::regex(
            R"(^[A-Za-z0-9\u00a1-\uffff](?:[A-Za-z0-9\u00a1-\uffff-]{0,61}[A-Za-z0-9\u00a1-\uffff])?(?:\.(?!-)[A-Za-z0-9\u00a1-\uffff-]{1,63}(?<!-))*\.?$)",
            std::regex_constants::icase);

        m_literal_re = std::regex(
            R"(\[([A-F0-9:.]+)\]$)",
            std::regex_constants::icase);
    }

    void operator()(const std::string& value) const {
        if (value.empty() || value.find('@') == std::string::npos || value.size() > 320) {
            throw ValidationError(m_message, m_code, value);
        }

        auto pos = value.rfind('@');
        std::string user_part   = value.substr(0, pos);
        std::string domain_part = value.substr(pos + 1);

        if (!std::regex_match(user_part, m_user_re)) {
            throw ValidationError(m_message, m_code, value);
        }

        if (std::find(m_domain_allowlist.begin(),
                      m_domain_allowlist.end(),
                      domain_part) != m_domain_allowlist.end())
        {
            return;
        }

        if (!validate_domain_part(domain_part)) {
            throw ValidationError(m_message, m_code, value);
        }
    }

private:
    bool validate_domain_part(const std::string& domain) const {

        if (std::regex_match(domain, m_domain_re)) {
            return true;
        }
        std::smatch sm;
        if (std::regex_match(domain, sm, m_literal_re)) {
            if (sm.size() > 1) {
                std::string ip = sm[1];
                return validate_ipv46_address(ip);
            }
        }
        return false;
    }

    static bool validate_ipv46_address(const std::string& ip) {

        struct in_addr addr4;
        if (inet_pton(AF_INET, ip.c_str(), &addr4) == 1) {
            return true;
        }

        struct in6_addr addr6;
        if (inet_pton(AF_INET6, ip.c_str(), &addr6) == 1) {
            return true;
        }
        return false;
    }

    std::string m_message;
    std::string m_code;
    std::vector<std::string> m_domain_allowlist;

    std::regex m_user_re;
    std::regex m_domain_re;
    std::regex m_literal_re;
};

class URLValidator : public RegexValidator {
public:
    URLValidator(const std::vector<std::string>& schemes = {"http","https","ftp","ftps"},
                 const std::string& message = "Enter a valid URL.",
                 const std::string& code    = "invalid")
        : RegexValidator(
              "^(?:[a-z0-9.+-]*)://"
              "(?:[^\\s:@/]+(?::[^\\s:@/]*)?@)?"
              "(?:"
                  "(?:0|25[0-5]|2[0-4]\\d|1\\d?\\d?|[1-9]\\d?)"
                  "(?:\\.(?:0|25[0-5]|2[0-4]\\d|1\\d?\\d?|[1-9]\\d?)){3}"
                  "|\\[[0-9a-fA-F:.]+\\]"
                  "|[a-z\\u00a1-\\uffff0-9](?:[a-z\\u00a1-\\uffff0-9-]{0,61}[a-z\\u00a1-\\uffff0-9])?"
                     "(?:\\.(?!-)[a-z\\u00a1-\\uffff0-9-]{1,63}(?<!-))*"
                     "\\.?"
                  "|localhost"
              ")"
              "(?::[0-9]{1,5})?"
              "(?:[/?#][^\\s]*)?"
              "\\Z",
              message,
              code,
              false,
              std::regex_constants::icase),
          m_schemes(schemes)
    {}

    void operator()(const std::string& value) const override {
        if (value.size() > MAX_LENGTH) {
            throw ValidationError(m_message, m_code, value);
        }

        for (char c : value) {
            if (c == '\t' || c == '\r' || c == '\n') {
                throw ValidationError(m_message, m_code, value);
            }
        }

        auto pos = value.find("://");
        if (pos == std::string::npos) {
            throw ValidationError(m_message, m_code, value);
        }
        std::string scheme = value.substr(0, pos);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(), ::tolower);

        bool allowed = false;
        for (auto& s : m_schemes) {
            if (scheme == s) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            throw ValidationError(m_message, m_code, value);
        }

        RegexValidator::operator()(value);
    }

private:
    static const size_t MAX_LENGTH = 2048;
    std::vector<std::string> m_schemes;
};

class DomainNameValidator : public RegexValidator {
public:
    static constexpr size_t MAX_LENGTH = 255;

    DomainNameValidator(bool accept_idna = true,
                        const std::string& message = "Enter a valid domain name.",
                        const std::string& code    = "invalid")
        : RegexValidator("",
                         message,
                         code,
                         false,
                         std::regex_constants::icase),
          m_accept_idna(accept_idna)
    {

        static const std::string hostname_re =
            "[a-z\\u00a1-\\uffff0-9](?:[a-z\\u00a1-\\uffff0-9-]{0,61}[a-z\\u00a1-\\uffff0-9])?";
        static const std::string domain_re =
            "(?:\\.(?!-)[a-z\\u00a1-\\uffff0-9-]{1,63}(?<!-))*";
        static const std::string tld_re =
            "\\.(?!-)(?:[a-z\\u00a1-\\uffff-]{2,63}|xn--[a-z0-9]{1,59})(?<!-)\\.?";

        static const std::string ascii_hostname_re =
            "[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?";
        static const std::string ascii_domain_re =
            "(?:\\.(?!-)[a-zA-Z0-9-]{1,63}(?<!-))*";
        static const std::string ascii_tld_re =
            "\\.(?!-)(?:[a-zA-Z0-9-]{2,63})(?<!-)\\.?";

        if (m_accept_idna) {
            m_pattern = "^" + hostname_re + domain_re + tld_re + "$";
        } else {
            m_pattern = "^" + ascii_hostname_re + ascii_domain_re + ascii_tld_re + "$";
        }
    }

    void operator()(const std::string& value) const override {
        if (value.size() > MAX_LENGTH) {
            throw ValidationError(m_message, m_code, value);
        }
        if (!m_accept_idna) {

            for (char c : value) {
                if (static_cast<unsigned char>(c) > 127) {
                    throw ValidationError(m_message, m_code, value);
                }
            }
        }

        RegexValidator::operator()(value);
    }

private:
    bool m_accept_idna;
};

static const EmailValidator   g_emailValidator;
static const URLValidator     g_urlValidator;
static const DomainNameValidator g_domainValidator;

inline bool isEmail(const std::string& text) {
    try {
        g_emailValidator(text);
        return true;
    } catch (...) {
        return false;
    }
}

inline bool isURL(const std::string& text) {
    try {
        g_urlValidator(text);
        return true;
    } catch (...) {
        return false;
    }
}

inline bool isDomain(const std::string& text) {
    try {
        g_domainValidator(text);
        return true;
    } catch (...) {
        return false;
    }
}
