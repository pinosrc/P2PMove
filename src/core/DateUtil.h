// =============================================================================
//  DateUtil.h — date civili senza dipendenze da fuso orario
//  (algoritmi days_from_civil / civil_from_days di H. Hinnant)
// =============================================================================
#pragma once

#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <string>

namespace p2p {

struct Ymd {
    int y = 1970, m = 1, d = 1;

    // giorni dal 1970-01-01
    long toDays() const {
        const int yy = y - (m <= 2);
        const long era = (yy >= 0 ? yy : yy - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(yy - era * 400);
        const unsigned mp  = static_cast<unsigned>(m > 2 ? m - 3 : m + 9);
        const unsigned doy = (153 * mp + 2) / 5 + static_cast<unsigned>(d) - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<long>(doe) - 719468;
    }

    static Ymd fromDays(long z) {
        z += 719468;
        const long era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp  = (5 * doy + 2) / 153;
        Ymd r;
        r.d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
        r.m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
        r.y = static_cast<int>(yoe) + static_cast<int>(era) * 400 + (r.m <= 2);
        return r;
    }

    Ymd addDays(long n) const { return fromDays(toDays() + n); }

    static Ymd today() {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
    }

    // "AAAA-MM-GG" (per l'interfaccia)
    std::string iso() const {
        char b[32];
        std::snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
        return b;
    }

    // "AAAAMMGG" (formato DICOM DA)
    std::string dicom() const {
        char b[32];
        std::snprintf(b, sizeof b, "%04d%02d%02d", y, m, d);
        return b;
    }

    // accetta "AAAA-MM-GG" oppure "AAAAMMGG"; verifica che la data esista
    static bool parse(const std::string& s, Ymd& out) {
        Ymd r;
        // solo cifre (e i due trattini): sscanf da solo accetta spazi, segni e testo finale
        auto digitsAt = [&](std::initializer_list<int> pos) {
            for (int p : pos) if (s[p] < '0' || s[p] > '9') return false;
            return true;
        };
        if (s.size() == 10 && s[4] == '-' && s[7] == '-' && digitsAt({0, 1, 2, 3, 5, 6, 8, 9})) {
            std::sscanf(s.c_str(), "%4d-%2d-%2d", &r.y, &r.m, &r.d);
        } else if (s.size() == 8 && digitsAt({0, 1, 2, 3, 4, 5, 6, 7})) {
            std::sscanf(s.c_str(), "%4d%2d%2d", &r.y, &r.m, &r.d);
        } else {
            return false;
        }
        if (r.y < 1900 || r.y > 2200 || r.m < 1 || r.m > 12 || r.d < 1 || r.d > 31) return false;
        if (fromDays(r.toDays()).m != r.m) return false;   // es. 31 febbraio
        out = r;
        return true;
    }

    bool operator<=(const Ymd& o) const { return toDays() <= o.toDays(); }
};

// "AAAAMMGG" -> "GG/MM/AAAA" per la visualizzazione
inline std::string dicomDateToDisplay(const std::string& da) {
    if (da.size() != 8) return da;
    return da.substr(6, 2) + "/" + da.substr(4, 2) + "/" + da.substr(0, 4);
}

// timestamp locale "AAAA-MM-GG hh:mm:ss"
inline std::string nowTimestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

} // namespace p2p
