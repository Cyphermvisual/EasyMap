#include "warp.h"
#include <cmath>
#include <algorithm>

namespace {
    bool SolveLinearSystem8x8(double A[8][8], double B[8], double X[8]) {
        // Forward elimination with partial pivoting
        for (int i = 0; i < 8; ++i) {
            // Find pivot row
            int maxRow = i;
            double maxVal = std::abs(A[i][i]);
            for (int r = i + 1; r < 8; ++r) {
                if (std::abs(A[r][i]) > maxVal) {
                    maxVal = std::abs(A[r][i]);
                    maxRow = r;
                }
            }

            // Swap pivot row
            if (maxRow != i) {
                for (int c = 0; c < 8; ++c) {
                    std::swap(A[i][c], A[maxRow][c]);
                }
                std::swap(B[i], B[maxRow]);
            }

            // Check if singular
            if (std::abs(A[i][i]) < 1e-9) {
                return false;
            }

            // Pivot
            for (int r = i + 1; r < 8; ++r) {
                double factor = A[r][i] / A[i][i];
                for (int c = i; c < 8; ++c) {
                    A[r][c] -= factor * A[i][c];
                }
                B[r] -= factor * B[i];
            }
        }

        // Back substitution
        for (int i = 7; i >= 0; --i) {
            double sum = 0.0;
            for (int c = i + 1; c < 8; ++c) {
                sum += A[i][c] * X[c];
            }
            X[i] = (B[i] - sum) / A[i][i];
        }
        return true;
    }
}

bool ComputeHomography(const Point2D screenCorners[4], const Point2D textureCorners[4], HomographyMatrix& outH) {
    // We want to map screen (x, y) to texture (u, v)
    // The model is:
    // u = (h00*x + h01*y + h02) / (h20*x + h21*y + 1)
    // v = (h10*x + h11*y + h12) / (h20*x + h21*y + 1)
    //
    // This gives two equations per point:
    // h00*x + h01*y + h02 - h20*x*u - h21*y*u = u
    // h10*x + h11*y + h12 - h20*x*v - h21*y*v = v
    
    double A[8][8] = { 0 };
    double B[8] = { 0 };
    
    for (int i = 0; i < 4; ++i) {
        double x = screenCorners[i].x;
        double y = screenCorners[i].y;
        double u = textureCorners[i].x;
        double v = textureCorners[i].y;
        
        int r1 = i * 2;
        int r2 = i * 2 + 1;
        
        // Equation 1: for u
        A[r1][0] = x;
        A[r1][1] = y;
        A[r1][2] = 1.0;
        A[r1][3] = 0.0;
        A[r1][4] = 0.0;
        A[r1][5] = 0.0;
        A[r1][6] = -x * u;
        A[r1][7] = -y * u;
        B[r1] = u;
        
        // Equation 2: for v
        A[r2][0] = 0.0;
        A[r2][1] = 0.0;
        A[r2][2] = 0.0;
        A[r2][3] = x;
        A[r2][4] = y;
        A[r2][5] = 1.0;
        A[r2][6] = -x * v;
        A[r2][7] = -y * v;
        B[r2] = v;
    }
    
    double h[8] = { 0 };
    if (!SolveLinearSystem8x8(A, B, h)) {
        return false;
    }
    
    // Fill the output 3x3 matrix
    outH.m[0][0] = static_cast<float>(h[0]);
    outH.m[0][1] = static_cast<float>(h[1]);
    outH.m[0][2] = static_cast<float>(h[2]);
    
    outH.m[1][0] = static_cast<float>(h[3]);
    outH.m[1][1] = static_cast<float>(h[4]);
    outH.m[1][2] = static_cast<float>(h[5]);
    
    outH.m[2][0] = static_cast<float>(h[6]);
    outH.m[2][1] = static_cast<float>(h[7]);
    outH.m[2][2] = 1.0f;
    
    return true;
}
