struct VSIn
{
    float2 pos  : POSITION;
    float4 col  : COLOR0;
    float2 ipos : IPOS;
    float  iang : IANGLE;
    float  iscl : ISCALE;
};
struct VSOut
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
};
VSOut VSMain(VSIn i)
{
    float s = sin(i.iang);
    float c = cos(i.iang);
    float2 p = float2(i.pos.x * c - i.pos.y * s, i.pos.x * s + i.pos.y * c);
    p = p * i.iscl + i.ipos;
    VSOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.col = i.col;
    return o;
}
float4 PSMain(VSOut i) : SV_TARGET
{
    return i.col;
}