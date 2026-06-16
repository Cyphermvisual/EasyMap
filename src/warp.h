#pragma once

struct Point2D {
    float x;
    float y;
};

struct WarpSettings {
    Point2D corners[4]; // 0: Top-Left, 1: Top-Right, 2: Bottom-Right, 3: Bottom-Left
};

struct HomographyMatrix {
    float m[3][3];
};

// Computes the homography matrix H that maps destination coordinates (screen/projector pixels)
// to source coordinates (texture coordinates [0, 1] x [0, 1]).
// Returns true on success, false if the system is degenerate.
bool ComputeHomography(const Point2D src[4], const Point2D dst[4], HomographyMatrix& outH);
