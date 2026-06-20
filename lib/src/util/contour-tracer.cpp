#include "util/contour-tracer.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <glm/glm.hpp>

namespace EdgeLighting
{
    namespace
    {
        // Corner order within a 2x2 cell:
        //   A(x,y) - B(x+1,y)
        //   C(x,y+1)- D(x+1,y+1)
        // Edges: 0=top(A-B), 1=right(B-D), 2=bottom(C-D), 3=left(A-C)

        // For each case (0-15), pairs of edges that form contour segments.
        // Cases 6 and 9 are saddle cells; entries here are the "valley" pairing.
        // At runtime, saddle ambiguity is resolved by comparing the average
        // corner alpha against the threshold (ridge = connect inside corners).
        const int MARCH_TABLE[16][4] = {
            {-1, -1, -1, -1}, // 0:  none
            {0, 3, -1, -1},   // 1:  A
            {0, 1, -1, -1},   // 2:  B
            {1, 3, -1, -1},   // 3:  A+B
            {2, 3, -1, -1},   // 4:  C
            {0, 2, -1, -1},   // 5:  A+C
            {0, 1, 2, 3},     // 6:  B+C (saddle — see resolveSaddle)
            {1, 2, -1, -1},   // 7:  A+B+C
            {1, 2, -1, -1},   // 8:  D
            {0, 1, 2, 3},     // 9:  A+D (saddle — see resolveSaddle)
            {0, 2, -1, -1},   // 10: B+D
            {2, 3, -1, -1},   // 11: A+B+D
            {1, 3, -1, -1},   // 12: C+D
            {0, 1, -1, -1},   // 13: A+C+D
            {0, 3, -1, -1},   // 14: B+C+D
            {-1, -1, -1, -1}, // 15: all
        };

        // Resolve saddle case pairing based on average corner alpha.
        // Returns a 4-element array with the chosen edge pairs.
        // saddleCase is 6 (B+C inside) or 9 (A+D inside).
        // Ridge (connect inside corners): pairs are (0,2),(1,3) for case 6;
        //                                         (1,2),(0,3) for case 9  [NO! actually it depends]
        //
        //                    A(x,y) -0- B(x+1,y)
        //                     |         |
        //                     3         1
        //                     |         |
        //                    C(x,y+1)-2- D(x+1,y+1)
        //
        // Case 6: B=1, C=1 inside. Inside corners are top-right and bottom-left.
        //   Ridge (center inside):  contour connects B→C across center → pairs (0,2) and (1,3)
        //   Valley (center outside): contour separates B from C → pairs (0,1) and (2,3)
        //
        // Case 9: A=1, D=1 inside. Inside corners are top-left and bottom-right.
        //   Ridge (center inside):  contour connects A→D across center → pairs (0,2) and (1,3)
        //   Valley (center outside): contour separates A from D → pairs (0,3) and (1,2)
        static void resolveSaddle(int caseIdx, float vA, float vB, float vC, float vD,
                                  float threshold, int outEdges[4])
        {
            float avg = (vA + vB + vC + vD) * 0.25f;
            if (avg >= threshold)
            {
                // Center is inside → ridge: connect inside corners through center
                outEdges[0] = 0;
                outEdges[1] = 2; // top↔bottom
                outEdges[2] = 1;
                outEdges[3] = 3; // right↔left
            }
            else
            {
                // Center is outside → valley: separate inside corners
                if (caseIdx == 6)
                {
                    outEdges[0] = 0;
                    outEdges[1] = 1; // top↔right
                    outEdges[2] = 2;
                    outEdges[3] = 3; // bottom↔left
                }
                else
                { // case 9
                    outEdges[0] = 0;
                    outEdges[1] = 3; // top↔left
                    outEdges[2] = 1;
                    outEdges[3] = 2; // right↔bottom
                }
            }
        }

        float lerpAlpha(float va, float vb, float threshold)
        {
            if (std::abs(vb - va) < 1e-7f)
            {
                return 0.5f;
            }
            return std::max(0.0f, std::min(1.0f, (threshold - va) / (vb - va)));
        }

        // Interpolated position on a cell edge given the four corner alpha values.
        glm::vec2 edgePos(int x, int y, int edge, float vA, float vB, float vC, float vD, float threshold)
        {
            float t;
            switch (edge)
            {
            case 0:
            {
                t = lerpAlpha(vA, vB, threshold);
                return {x + t, y};
            }
            case 1:
            {
                t = lerpAlpha(vB, vD, threshold);
                return {x + 1, y + t};
            }
            case 2:
            {
                t = lerpAlpha(vC, vD, threshold);
                return {x + t, y + 1};
            }
            case 3:
            {
                t = lerpAlpha(vA, vC, threshold);
                return {x, y + t};
            }
            default:
            {
                return {0, 0};
            }
            }
        }

        typedef struct Segment
        {
            glm::vec2 a, b;
        } Segment;

        // Chain segments into a single closed polyline by walking the adjacency graph.
        std::vector<glm::vec2> chainSegments(const std::vector<Segment> &segs, float snap)
        {
            if (segs.empty())
            {
                return {};
            }

            using Key = glm::ivec2;
            typedef struct Hasher
            {
                size_t operator()(const Key &k) const
                {
                    return static_cast<size_t>(k.x) ^ (static_cast<size_t>(k.y) << 16);
                }
            } Hasher;

            // Build adjacency: map snapped key → list of connected keys.
            // Also store one canonical position per key.
            std::unordered_map<Key, std::vector<Key>, Hasher> adj;
            std::unordered_map<Key, glm::vec2, Hasher> posMap;

            for (auto &s : segs)
            {
                Key ka = Key(static_cast<int>(std::round(s.a.x / snap)),
                             static_cast<int>(std::round(s.a.y / snap)));
                Key kb = Key(static_cast<int>(std::round(s.b.x / snap)),
                             static_cast<int>(std::round(s.b.y / snap)));

                adj[ka].push_back(kb);
                adj[kb].push_back(ka);
                if (!posMap.count(ka))
                {
                    posMap[ka] = s.a;
                }
                if (!posMap.count(kb))
                {
                    posMap[kb] = s.b;
                }
            }

            if (adj.empty())
            {
                return {};
            }

            // Pick start key (prefer a dead-end for open curves; any for closed)
            Key startKey = adj.begin()->first;
            for (auto &kv : adj)
            {
                if (kv.second.size() == 1)
                {
                    startKey = kv.first;
                    break;
                }
            }

            // Walk
            std::vector<glm::vec2> chain;
            chain.reserve(adj.size());
            chain.push_back(posMap[startKey]);

            Key curKey = startKey;
            Key prevKey(-1, -1); // sentinel — never matches a real key

            size_t maxSteps = adj.size() * 2;
            for (size_t step = 0; step < maxSteps; ++step)
            {
                auto &neighbors = adj[curKey];
                Key nextKey = curKey;

                for (auto &nk : neighbors)
                {
                    if (nk != prevKey)
                    {
                        nextKey = nk;
                        break;
                    }
                }

                if (nextKey == curKey)
                {
                    break; // dead end
                }

                // Check if we're about to return to start
                if (nextKey == startKey && chain.size() > 2)
                {
                    break; // closed loop complete
                }

                chain.push_back(posMap[nextKey]);
                prevKey = curKey;
                curKey = nextKey;
            }

            return chain;
        }

        std::vector<glm::vec2> resampleUniform(const std::vector<glm::vec2> &points, int maxCount)
        {
            if (points.size() <= static_cast<size_t>(maxCount))
            {
                return points;
            }

            std::vector<glm::vec2> result;
            result.reserve(maxCount);

            float step = static_cast<float>(points.size()) / static_cast<float>(maxCount);
            for (int i = 0; i < maxCount; ++i)
            {
                int idx = std::min(static_cast<int>(i * step), static_cast<int>(points.size()) - 1);
                result.push_back(points[idx]);
            }
            return result;
        }

    } // anonymous namespace

    std::vector<glm::vec2> TraceOutermostContour(
        const uint8_t *pixels, int imgW, int imgH,
        float rectW, float rectH,
        uint8_t alphaThreshold, int maxPoints)
    {
        if (!pixels || imgW <= 1 || imgH <= 1)
        {
            return {};
        }

        float threshold = static_cast<float>(alphaThreshold) / 255.0f;

        // --- Marching Squares: extract contour segments ---
        std::vector<Segment> segs;
        segs.reserve(static_cast<size_t>(imgW) * static_cast<size_t>(imgH));

        for (int y = 0; y < imgH - 1; ++y)
        {
            for (int x = 0; x < imgW - 1; ++x)
            {
                size_t idxA = static_cast<size_t>(y) * imgW + x;
                size_t idxB = idxA + 1;
                size_t idxC = (static_cast<size_t>(y) + 1) * imgW + x;
                size_t idxD = idxC + 1;

                float vA = pixels[idxA * 4 + 3] / 255.0f;
                float vB = pixels[idxB * 4 + 3] / 255.0f;
                float vC = pixels[idxC * 4 + 3] / 255.0f;
                float vD = pixels[idxD * 4 + 3] / 255.0f;

                int bA = (vA >= threshold) ? 1 : 0;
                int bB = (vB >= threshold) ? 1 : 0;
                int bC = (vC >= threshold) ? 1 : 0;
                int bD = (vD >= threshold) ? 1 : 0;
                int caseIdx = bA | (bB << 1) | (bC << 2) | (bD << 3);

                if (caseIdx == 0 || caseIdx == 15)
                {
                    continue;
                }

                int edges[4];
                if (caseIdx == 6 || caseIdx == 9)
                {
                    resolveSaddle(caseIdx, vA, vB, vC, vD, threshold, edges);
                }
                else
                {
                    for (int i = 0; i < 4; ++i)
                    {
                        edges[i] = MARCH_TABLE[caseIdx][i];
                    }
                }

                auto addSeg = [&](int e1, int e2)
                {
                    glm::vec2 p1 = edgePos(x, y, e1, vA, vB, vC, vD, threshold);
                    glm::vec2 p2 = edgePos(x, y, e2, vA, vB, vC, vD, threshold);
                    // Skip zero-length segments (can happen when alpha == threshold
                    // at a pixel corner, placing both endpoints at the same point)
                    float dx = p1.x - p2.x, dy = p1.y - p2.y;
                    if (dx * dx + dy * dy > 1e-8f)
                    {
                        segs.push_back({p1, p2});
                    }
                };

                if (edges[0] >= 0 && edges[1] >= 0)
                {
                    addSeg(edges[0], edges[1]);
                }
                if (edges[2] >= 0 && edges[3] >= 0)
                {
                    addSeg(edges[2], edges[3]);
                }
            }
        }

        if (segs.empty())
        {
            return {};
        }

        // --- Chain segments into a polyline ---
        float snap = 1.0f / std::max(imgW, imgH);
        auto contour = chainSegments(segs, snap);

        if (contour.empty())
        {
            return {};
        }

        // Remove duplicate start point at end if the loop closed
        if (contour.size() > 2)
        {
            float dx = contour.front().x - contour.back().x;
            float dy = contour.front().y - contour.back().y;
            if (dx * dx + dy * dy < 0.01f)
            {
                contour.pop_back();
            }
        }

        // --- Uniform resampling if too many points ---
        auto sampled = resampleUniform(contour, maxPoints);

        // --- Convert pixel coords to app space ---
        std::vector<glm::vec2> result;
        result.reserve(sampled.size());
        float scaleX = rectW / static_cast<float>(imgW);
        float scaleY = rectH / static_cast<float>(imgH);
        for (const auto &p : sampled)
        {
            result.push_back({p.x * scaleX, p.y * scaleY});
        }

        return result;
    }

} // namespace EdgeLighting
