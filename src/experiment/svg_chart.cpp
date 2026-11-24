#include "experiment/svg_chart.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace arq {

namespace {

const char* kPalette[] = {"#2563eb", "#dc2626", "#16a34a", "#9333ea", "#ea580c",
                          "#0891b2", "#ca8a04", "#db2777", "#4b5563", "#65a30d"};

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '&') o += "&amp;";
        else o += c;
    }
    return o;
}

std::string num(double v) {
    char b[32];
    if (v != 0.0 && (std::fabs(v) < 1e-3 || std::fabs(v) >= 1e6)) std::snprintf(b, sizeof(b), "%.0e", v);
    else std::snprintf(b, sizeof(b), "%g", v);
    return b;
}

double niceStep(double range, int ticks) {
    double raw = range / std::max(1, ticks);
    double mag = std::pow(10.0, std::floor(std::log10(raw)));
    double f = raw / mag;
    double nice = f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10;
    return nice * mag;
}

}  // namespace

std::string LineChart::toSvg(int W, int H) const {
    const double left = 78, right = 190, top = 46, bottom = 62;
    const double pw = W - left - right, ph = H - top - bottom;

    double xmin = std::numeric_limits<double>::max(), xmax = std::numeric_limits<double>::lowest();
    double ymin = std::numeric_limits<double>::max(), ymax = std::numeric_limits<double>::lowest();
    for (const auto& s : series)
        for (const auto& p : s.points) {
            if (logY && p.y <= 0) continue;
            xmin = std::min(xmin, p.x);
            xmax = std::max(xmax, p.x);
            ymin = std::min(ymin, p.y - p.err);
            ymax = std::max(ymax, p.y + p.err);
        }
    if (xmin > xmax) { xmin = 0; xmax = 1; ymin = 0; ymax = 1; }
    if (xmax == xmin) { xmin -= 1; xmax += 1; }

    double ly0 = 0, ly1 = 1;
    std::vector<double> yTicks;
    if (logY) {
        double lo = std::numeric_limits<double>::max();
        for (const auto& s : series)
            for (const auto& p : s.points)
                if (p.y > 0) lo = std::min(lo, p.y);
        if (lo == std::numeric_limits<double>::max()) lo = 1e-6;
        ly0 = std::floor(std::log10(lo));
        ly1 = std::ceil(std::log10(std::max(ymax, lo * 10)));
        if (ly1 <= ly0) ly1 = ly0 + 1;
        for (double e = ly0; e <= ly1 + 1e-9; e += 1) yTicks.push_back(std::pow(10.0, e));
    } else {
        if (yFromZero) ymin = std::min(0.0, ymin);
        if (ymax <= ymin) ymax = ymin + 1;
        double step = niceStep(ymax - ymin, 6);
        ymin = std::floor(ymin / step) * step;
        ymax = std::ceil(ymax / step) * step;
        for (double v = ymin; v <= ymax + step * 1e-6; v += step) yTicks.push_back(v);
    }
    double xstep = niceStep(xmax - xmin, 8);
    double xa = std::floor(xmin / xstep) * xstep, xb = std::ceil(xmax / xstep) * xstep;
    xmin = xa;
    xmax = xb;

    auto X = [&](double x) { return left + (x - xmin) / (xmax - xmin) * pw; };
    auto Y = [&](double y) {
        if (logY) {
            double l = std::log10(std::max(y, std::pow(10.0, ly0)));
            return top + ph - (l - ly0) / (ly1 - ly0) * ph;
        }
        return top + ph - (y - ymin) / (ymax - ymin) * ph;
    };

    std::ostringstream o;
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << W << "\" height=\"" << H << "\" viewBox=\"0 0 "
      << W << " " << H << "\" font-family=\"Helvetica, Arial, sans-serif\">\n";
    o << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";
    o << "<text x=\"" << W / 2 << "\" y=\"26\" text-anchor=\"middle\" font-size=\"16\" font-weight=\"bold\">"
      << esc(title) << "</text>\n";

    // grid + ticks
    for (double v : yTicks) {
        double y = Y(v);
        o << "<line x1=\"" << left << "\" y1=\"" << y << "\" x2=\"" << left + pw << "\" y2=\"" << y
          << "\" stroke=\"#e5e7eb\"/>\n";
        o << "<text x=\"" << left - 8 << "\" y=\"" << y + 4 << "\" text-anchor=\"end\" font-size=\"11\">" << num(v)
          << "</text>\n";
    }
    for (double v = xmin; v <= xmax + xstep * 1e-6; v += xstep) {
        double x = X(v);
        o << "<line x1=\"" << x << "\" y1=\"" << top << "\" x2=\"" << x << "\" y2=\"" << top + ph
          << "\" stroke=\"#f1f5f9\"/>\n";
        o << "<text x=\"" << x << "\" y=\"" << top + ph + 18 << "\" text-anchor=\"middle\" font-size=\"11\">"
          << num(v) << "</text>\n";
    }
    o << "<rect x=\"" << left << "\" y=\"" << top << "\" width=\"" << pw << "\" height=\"" << ph
      << "\" fill=\"none\" stroke=\"#374151\"/>\n";
    o << "<text x=\"" << left + pw / 2 << "\" y=\"" << H - 18 << "\" text-anchor=\"middle\" font-size=\"13\">"
      << esc(xLabel) << "</text>\n";
    o << "<text transform=\"translate(20," << top + ph / 2 << ") rotate(-90)\" text-anchor=\"middle\" "
      << "font-size=\"13\">" << esc(yLabel) << "</text>\n";

    // series
    for (std::size_t i = 0; i < series.size(); ++i) {
        const auto& s = series[i];
        const char* color = kPalette[i % (sizeof(kPalette) / sizeof(kPalette[0]))];
        std::vector<ChartPoint> pts = s.points;
        std::sort(pts.begin(), pts.end(), [](const ChartPoint& a, const ChartPoint& b) { return a.x < b.x; });
        std::ostringstream path;
        bool first = true;
        for (const auto& p : pts) {
            if (logY && p.y <= 0) continue;
            path << (first ? "M" : " L") << X(p.x) << "," << Y(p.y);
            first = false;
        }
        o << "<path d=\"" << path.str() << "\" fill=\"none\" stroke=\"" << color << "\" stroke-width=\"2\""
          << (s.dashed ? " stroke-dasharray=\"6,4\"" : "") << "/>\n";
        for (const auto& p : pts) {
            if (logY && p.y <= 0) continue;
            double cx = X(p.x), cy = Y(p.y);
            if (p.err > 0) {
                double y0 = Y(p.y - p.err), y1 = Y(p.y + p.err);
                o << "<line x1=\"" << cx << "\" y1=\"" << y0 << "\" x2=\"" << cx << "\" y2=\"" << y1
                  << "\" stroke=\"" << color << "\"/>\n";
                o << "<line x1=\"" << cx - 4 << "\" y1=\"" << y0 << "\" x2=\"" << cx + 4 << "\" y2=\"" << y0
                  << "\" stroke=\"" << color << "\"/>\n";
                o << "<line x1=\"" << cx - 4 << "\" y1=\"" << y1 << "\" x2=\"" << cx + 4 << "\" y2=\"" << y1
                  << "\" stroke=\"" << color << "\"/>\n";
            }
            o << "<circle cx=\"" << cx << "\" cy=\"" << cy << "\" r=\"3.5\" fill=\"" << color << "\"/>\n";
        }
        double ly = top + 10 + double(i) * 20;
        o << "<line x1=\"" << left + pw + 14 << "\" y1=\"" << ly << "\" x2=\"" << left + pw + 38 << "\" y2=\""
          << ly << "\" stroke=\"" << color << "\" stroke-width=\"2\""
          << (s.dashed ? " stroke-dasharray=\"6,4\"" : "") << "/>\n";
        o << "<text x=\"" << left + pw + 44 << "\" y=\"" << ly + 4 << "\" font-size=\"12\">" << esc(s.name)
          << "</text>\n";
    }
    o << "</svg>\n";
    return o.str();
}

void LineChart::save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f << toSvg();
}

std::string asciiBars(const std::vector<std::pair<std::string, double>>& rows, const std::string& unit, int width) {
    double mx = 0;
    std::size_t labelW = 0;
    for (const auto& r : rows) {
        mx = std::max(mx, r.second);
        labelW = std::max(labelW, r.first.size());
    }
    std::ostringstream o;
    for (const auto& r : rows) {
        int n = mx > 0 ? int(std::lround(r.second / mx * width)) : 0;
        o << "  " << r.first << std::string(labelW - r.first.size(), ' ') << " |" << std::string(std::size_t(n), '#')
          << std::string(std::size_t(width - n), ' ') << "| ";
        char b[64];
        std::snprintf(b, sizeof(b), "%.1f %s", r.second, unit.c_str());
        o << b << "\n";
    }
    return o.str();
}

}  // namespace arq
