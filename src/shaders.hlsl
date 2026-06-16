cbuffer ConstantBuffer : register(b0)
{
    float4 HRow0;       // Homography row 0 (xyz)
    float4 HRow1;       // Homography row 1 (xyz)
    float4 HRow2;       // Homography row 2 (xyz)
    float4 GridColor;   // (r, g, b, a)
    float4 GridSettings;// x: ShowGrid (1 or 0), y: GridCols, z: GridRows, w: BorderThickness (pixels)
    
    // Calibration guides (rendered in pixels)
    float4 Corner0;     // xy: corner0 pixel pos
    float4 Corner1;     // xy: corner1 pixel pos
    float4 Corner2;     // xy: corner2 pixel pos
    float4 Corner3;     // xy: corner3 pixel pos
    float4 GuideSettings; // x: ShowGuides (1 or 0), y: SelectedCorner (0-3, or -1), z: HandleRadius (pixels), w: LineThickness (pixels)
    float4 GuideColor;  // Color for lines/unselected handles (r, g, b, a)
    float4 ActiveGuideColor; // Color for selected handle (r, g, b, a)
};

Texture2D shaderTexture : register(t0);
SamplerState sampleState : register(s0);

struct VS_OUTPUT
{
    float4 Pos : SV_POSITION;
};

// Generate full-screen quad from Vertex ID
VS_OUTPUT VS(uint VertexID : SV_VertexID)
{
    VS_OUTPUT output;
    float2 grid[4] = {
        float2(-1.0f,  1.0f), // TL
        float2( 1.0f,  1.0f), // TR
        float2(-1.0f, -1.0f), // BL
        float2( 1.0f, -1.0f)  // BR
    };
    output.Pos = float4(grid[VertexID], 0.0f, 1.0f);
    return output;
}

// Distance from point p to line segment ab
float DistToSegment(float2 p, float2 a, float2 b)
{
    float2 ap = p - a;
    float2 ab = b - a;
    float t = clamp(dot(ap, ab) / dot(ab, ab), 0.0f, 1.0f);
    return distance(p, a + t * ab);
}

float4 PS(VS_OUTPUT input) : SV_TARGET
{
    float2 pixelPos = input.Pos.xy;
    float3 screenPos = float3(pixelPos, 1.0f);
    
    // 1. Calculate Homography Map
    float u = dot(HRow0.xyz, screenPos);
    float v = dot(HRow1.xyz, screenPos);
    float w = dot(HRow2.xyz, screenPos);
    float2 uv = float2(u, v) / w;
    
    float4 finalColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
    bool insideWarp = (uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f);
    
    // Sample texture if inside the warp bounds
    if (insideWarp)
    {
        finalColor = shaderTexture.Sample(sampleState, uv);
        
        // Render calibration grid if enabled
        if (GridSettings.x > 0.5f)
        {
            float cols = GridSettings.y;
            float rows = GridSettings.z;
            float thickness = GridSettings.w;
            
            float2 cellUV = uv * float2(cols, rows);
            float2 distToGrid = abs(frac(cellUV + 0.5f) - 0.5f) / float2(cols, rows);
            
            float minDistX = min(uv.x, 1.0f - uv.x);
            float minDistY = min(uv.y, 1.0f - uv.y);
            float borderDistX = min(distToGrid.x, minDistX);
            float borderDistY = min(distToGrid.y, minDistY);
            
            float2 uvDeriv = fwidth(uv);
            float2 threshold = uvDeriv * thickness;
            
            if (borderDistX < threshold.x || borderDistY < threshold.y)
            {
                finalColor = lerp(finalColor, float4(GridColor.rgb, 1.0f), GridColor.a);
            }
        }
    }
    
    // 2. Render Calibration Guides (Overlay) on top
    if (GuideSettings.x > 0.5f)
    {
        float radius = GuideSettings.z;
        float lineThickness = GuideSettings.w;
        int selectedIndex = (int)(GuideSettings.y + 0.5f);
        
        // Check distance to corners (handles)
        float d0 = distance(pixelPos, Corner0.xy);
        float d1 = distance(pixelPos, Corner1.xy);
        float d2 = distance(pixelPos, Corner2.xy);
        float d3 = distance(pixelPos, Corner3.xy);
        
        // Check distance to boundary lines
        float dLine = min(
            min(DistToSegment(pixelPos, Corner0.xy, Corner1.xy), DistToSegment(pixelPos, Corner1.xy, Corner2.xy)),
            min(DistToSegment(pixelPos, Corner2.xy, Corner3.xy), DistToSegment(pixelPos, Corner3.xy, Corner0.xy))
        );
        
        // Overlay lines
        if (dLine < lineThickness)
        {
            // Simple anti-aliasing for the line edge
            float alpha = clamp((lineThickness - dLine), 0.0f, 1.0f) * GuideColor.a;
            finalColor = lerp(finalColor, float4(GuideColor.rgb, 1.0f), alpha);
        }
        
        // Overlay circles (handles)
        float dCircle = min(min(d0, d1), min(d2, d3));
        if (dCircle < radius)
        {
            // Check if we are drawing the selected handle
            bool isSelected = false;
            if (selectedIndex == 0 && d0 < radius) isSelected = true;
            if (selectedIndex == 1 && d1 < radius) isSelected = true;
            if (selectedIndex == 2 && d2 < radius) isSelected = true;
            if (selectedIndex == 3 && d3 < radius) isSelected = true;
            
            float4 color = isSelected ? ActiveGuideColor : GuideColor;
            
            // Draw handle center
            if (dCircle < radius - 2.0f)
            {
                finalColor = lerp(finalColor, float4(color.rgb, 1.0f), color.a);
            }
            // Draw handle border (darker outline for visibility)
            else
            {
                finalColor = lerp(finalColor, float4(0.0f, 0.0f, 0.0f, 1.0f), 0.8f);
            }
        }
    }
    
    return finalColor;
}
