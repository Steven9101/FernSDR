// Tiny assertion harness: no framework, no dependencies, just enough to make
// failures readable and give the binary a non-zero exit status.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

namespace test {

inline int& failures() { static int f = 0; return f; }
inline int& checks() { static int c = 0; return c; }
inline const char*& current_case() { static const char* c = "?"; return c; }

inline void fail(const char* file, int line, const std::string& msg) {
    failures()++;
    fprintf(stderr, "  FAIL %s:%d [%s] %s\n", file, line, current_case(), msg.c_str());
}

inline void check_true(bool cond, const char* expr, const char* file, int line) {
    checks()++;
    if (!cond) fail(file, line, std::string("expected true: ") + expr);
}

inline void check_near(double got, double want, double tol, const char* expr, const char* file, int line) {
    checks()++;
    if (!(std::fabs(got - want) <= tol)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: got %.9g want %.9g (tol %.3g)", expr, got, want, tol);
        fail(file, line, buf);
    }
}

inline void check_eq_int(long long got, long long want, const char* expr, const char* file, int line) {
    checks()++;
    if (got != want) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: got %lld want %lld", expr, got, want);
        fail(file, line, buf);
    }
}

// Integers compare as long long, as before. A floating-point operand used to
// be truncated to an integer too, so CHECK_EQ(x, 0.5f) passed for any x in
// [0, 1); it now compares exactly.
template <class Got, class Want>
inline void check_eq(Got got, Want want, const char* expr, const char* file, int line) {
    if constexpr (std::is_floating_point_v<Got> || std::is_floating_point_v<Want>) {
        checks()++;
        if (!(static_cast<double>(got) == static_cast<double>(want))) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s: got %.9g want %.9g", expr, static_cast<double>(got),
                     static_cast<double>(want));
            fail(file, line, buf);
        }
    } else {
        check_eq_int(static_cast<long long>(got), static_cast<long long>(want), expr, file, line);
    }
}

inline void check_eq_str(const std::string& got, const std::string& want, const char* expr,
                         const char* file, int line) {
    checks()++;
    if (got != want) fail(file, line, std::string(expr) + ": got \"" + got + "\" want \"" + want + "\"");
}

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

// `filter`, when given, runs only the cases whose name contains it.
inline int run_all(const char* filter = nullptr) {
    for (auto& c : registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        current_case() = c.name;
        int before = failures();
        c.fn();
        printf("  %s %s\n", failures() == before ? "ok  " : "FAIL", c.name);
    }
    printf("%d checks, %d failures\n", checks(), failures());
    return failures() == 0 ? 0 : 1;
}

}  // namespace test

#define TEST_CASE(name)                                                  \
    static void name();                                                  \
    static ::test::Registrar reg_##name(#name, name);                    \
    static void name()

#define CHECK(cond) ::test::check_true((cond), #cond, __FILE__, __LINE__)
#define CHECK_NEAR(got, want, tol) ::test::check_near((got), (want), (tol), #got, __FILE__, __LINE__)
#define CHECK_EQ_STR(got, want) ::test::check_eq_str((got), (want), #got, __FILE__, __LINE__)
#define CHECK_EQ(got, want) ::test::check_eq((got), (want), #got, __FILE__, __LINE__)
