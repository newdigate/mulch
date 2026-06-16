#include "gfx/TextGeometry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#include <mapbox/earcut.hpp>

// Teach earcut how to read a std::array<double,2> point.
namespace mapbox { namespace util {
template <> struct nth<0, std::array<double, 2>> {
    static double get(const std::array<double, 2>& p) { return p[0]; }
};
template <> struct nth<1, std::array<double, 2>> {
    static double get(const std::array<double, 2>& p) { return p[1]; }
};
}} // namespace mapbox::util

namespace oss {
namespace {

using Pt = std::array<double, 2>;

struct Contour {
    std::vector<Pt> pts;
    double          area    = 0.0;   // signed; sign encodes winding
    bool            isOuter = false;
};

std::vector<unsigned char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f),
                                      std::istreambuf_iterator<char>());
}

double signedArea(const std::vector<Pt>& c) {
    double a = 0.0;
    for (std::size_t i = 0, n = c.size(); i < n; ++i) {
        const Pt& p = c[i];
        const Pt& q = c[(i + 1) % n];
        a += p[0] * q[1] - q[0] * p[1];
    }
    return 0.5 * a;
}

bool pointInPolygon(const Pt& pt, const std::vector<Pt>& poly) {
    bool inside = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        double xi = poly[i][0], yi = poly[i][1], xj = poly[j][0], yj = poly[j][1];
        if (((yi > pt[1]) != (yj > pt[1])) &&
            (pt[0] < (xj - xi) * (pt[1] - yi) / (yj - yi) + xi))
            inside = !inside;
    }
    return inside;
}

// Flatten one glyph's stb outline (lines + quadratic/cubic beziers) into closed
// contours, offset horizontally by the pen position. Points are in font units.
void flattenGlyph(const stbtt_vertex* verts, int nverts, double penX,
                  std::vector<Contour>& out) {
    const int STEPS = 8;
    std::vector<Pt> cur;
    double lastx = 0, lasty = 0;
    auto flush = [&] { if (cur.size() >= 3) { Contour c; c.pts = cur; out.push_back(std::move(c)); } cur.clear(); };

    for (int i = 0; i < nverts; ++i) {
        const stbtt_vertex& v = verts[i];
        double vx = v.x, vy = v.y, cx = v.cx, cy = v.cy, cx1 = v.cx1, cy1 = v.cy1;
        switch (v.type) {
            case STBTT_vmove:
                flush();
                cur.push_back({penX + vx, vy});
                break;
            case STBTT_vline:
                cur.push_back({penX + vx, vy});
                break;
            case STBTT_vcurve:                       // quadratic bezier
                for (int s = 1; s <= STEPS; ++s) {
                    double t = (double)s / STEPS, u = 1 - t;
                    double x = u * u * lastx + 2 * u * t * cx + t * t * vx;
                    double y = u * u * lasty + 2 * u * t * cy + t * t * vy;
                    cur.push_back({penX + x, y});
                }
                break;
            case STBTT_vcubic:                       // cubic bezier
                for (int s = 1; s <= STEPS; ++s) {
                    double t = (double)s / STEPS, u = 1 - t;
                    double x = u*u*u*lastx + 3*u*u*t*cx + 3*u*t*t*cx1 + t*t*t*vx;
                    double y = u*u*u*lasty + 3*u*u*t*cy + 3*u*t*t*cy1 + t*t*t*vy;
                    cur.push_back({penX + x, y});
                }
                break;
        }
        lastx = vx; lasty = vy;
    }
    flush();
}

} // namespace

TextGeometry buildTextGeometry(const std::string& text, const std::string& fontPath,
                               float size, float depth) {
    TextGeometry g;

    std::vector<unsigned char> font = readFile(fontPath);
    if (font.empty()) { g.error = "could not open font: " + fontPath; return g; }

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, font.data(), stbtt_GetFontOffsetForIndex(font.data(), 0))) {
        g.error = "could not parse font: " + fontPath;
        return g;
    }
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
    double emHeight = (ascent - descent) > 0 ? (ascent - descent) : 1000.0;

    // --- Lay out the string into contours (font units) ---
    std::vector<Contour> contours;
    double penX = 0.0;
    for (unsigned char ch : text) {                 // ASCII / Latin-1 per byte
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&info, ch, &adv, &lsb);
        stbtt_vertex* verts = nullptr;
        int nv = stbtt_GetCodepointShape(&info, ch, &verts);
        if (nv > 0 && verts) flattenGlyph(verts, nv, penX, contours);
        if (verts) stbtt_FreeShape(&info, verts);
        penX += adv;
    }
    if (contours.empty()) { g.ok = true; return g; }   // empty / whitespace -> nothing to draw

    // --- Classify contours as outer vs hole by winding, normalise orientation
    //     so outers are CCW and holes CW ---
    double maxAbs = 0.0, outerSign = 1.0;
    for (auto& c : contours) {
        c.area = signedArea(c.pts);
        if (std::fabs(c.area) > maxAbs) { maxAbs = std::fabs(c.area); outerSign = c.area >= 0 ? 1 : -1; }
    }
    for (auto& c : contours) {
        c.isOuter = ((c.area >= 0 ? 1.0 : -1.0) == outerSign) && std::fabs(c.area) > 1e-6;
        bool ccw = c.area > 0;
        if (c.isOuter != ccw) {                      // outer must be CCW, hole CW
            std::reverse(c.pts.begin(), c.pts.end());
            c.area = -c.area;
        }
    }

    // --- Assign each hole to the smallest outer that contains it ---
    std::vector<int> outers, holes;
    for (int i = 0; i < (int)contours.size(); ++i) (contours[i].isOuter ? outers : holes).push_back(i);
    std::vector<std::vector<int>> outerHoles(outers.size());
    for (int h : holes) {
        int best = -1; double bestArea = 1e300;
        for (int oi = 0; oi < (int)outers.size(); ++oi) {
            if (pointInPolygon(contours[h].pts[0], contours[outers[oi]].pts)) {
                double ar = std::fabs(contours[outers[oi]].area);
                if (ar < bestArea) { bestArea = ar; best = oi; }
            }
        }
        if (best >= 0) outerHoles[best].push_back(h);
    }

    // --- Triangulate each outer+holes group into CCW front-face triangles ---
    std::vector<Pt> frontTri;   // every 3 entries = one triangle, CCW
    for (int oi = 0; oi < (int)outers.size(); ++oi) {
        std::vector<std::vector<Pt>> polygon;
        polygon.push_back(contours[outers[oi]].pts);
        for (int h : outerHoles[oi]) polygon.push_back(contours[h].pts);

        std::vector<Pt> flat;
        for (auto& ring : polygon) flat.insert(flat.end(), ring.begin(), ring.end());

        std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(polygon);
        for (std::size_t k = 0; k + 2 < idx.size(); k += 3) {
            Pt a = flat[idx[k]], b = flat[idx[k + 1]], c = flat[idx[k + 2]];
            double cross = (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0]);
            if (cross < 0) std::swap(b, c);          // force CCW
            frontTri.push_back(a); frontTri.push_back(b); frontTri.push_back(c);
        }
    }

    // --- Scale + centre: em height -> `size`, bounding box centred on origin ---
    double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
    for (auto& c : contours) for (auto& p : c.pts) {
        minX = std::min(minX, p[0]); maxX = std::max(maxX, p[0]);
        minY = std::min(minY, p[1]); maxY = std::max(maxY, p[1]);
    }
    double scale = (double)size / emHeight;
    double cx = 0.5 * (minX + maxX), cy = 0.5 * (minY + maxY);
    double zf = 0.5 * (double)depth;
    auto X = [&](double x) { return (x - cx) * scale; };
    auto Y = [&](double y) { return (y - cy) * scale; };

    auto pushV = [&](double x, double y, double z, double nx, double ny, double nz) {
        g.tris.insert(g.tris.end(), {(float)x,(float)y,(float)z,(float)nx,(float)ny,(float)nz});
    };
    auto pushL = [&](double x0,double y0,double z0,double x1,double y1,double z1) {
        g.lines.insert(g.lines.end(), {(float)x0,(float)y0,(float)z0,(float)x1,(float)y1,(float)z1});
    };

    // Front (and, when extruded, back) faces.
    for (std::size_t k = 0; k + 2 < frontTri.size(); k += 3) {
        double x0=X(frontTri[k][0]),  y0=Y(frontTri[k][1]);
        double x1=X(frontTri[k+1][0]),y1=Y(frontTri[k+1][1]);
        double x2=X(frontTri[k+2][0]),y2=Y(frontTri[k+2][1]);
        pushV(x0,y0,zf,0,0,1); pushV(x1,y1,zf,0,0,1); pushV(x2,y2,zf,0,0,1);
        if (depth > 0.0f) {                           // back: reversed winding, -Z normal
            pushV(x0,y0,-zf,0,0,-1); pushV(x2,y2,-zf,0,0,-1); pushV(x1,y1,-zf,0,0,-1);
        }
    }

    // Outline edges (always) and side walls (extruded only).
    for (auto& c : contours) {
        std::size_t n = c.pts.size();
        for (std::size_t i = 0; i < n; ++i) {
            double ax=X(c.pts[i][0]),       ay=Y(c.pts[i][1]);
            double bx=X(c.pts[(i+1)%n][0]), by=Y(c.pts[(i+1)%n][1]);
            pushL(ax,ay,zf,bx,by,zf);
            if (depth > 0.0f) {
                pushL(ax,ay,-zf,bx,by,-zf);
                double ex=bx-ax, ey=by-ay, len=std::sqrt(ex*ex+ey*ey);
                double nx = len>1e-9 ? ey/len : 0.0, ny = len>1e-9 ? -ex/len : 0.0;
                pushV(ax,ay,zf,nx,ny,0); pushV(bx,by,zf,nx,ny,0); pushV(bx,by,-zf,nx,ny,0);
                pushV(ax,ay,zf,nx,ny,0); pushV(bx,by,-zf,nx,ny,0); pushV(ax,ay,-zf,nx,ny,0);
            }
        }
    }

    g.ok = true;
    return g;
}

} // namespace oss
