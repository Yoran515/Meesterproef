#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "Player.h"
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

enum class GameState { Menu, Playing, Win };

struct Platform {
    Vector3 pos, size;
    Color   top, side;
    int     modeVis = 0;
    bool    solid   = true;
    float   origZ   = 0;

    // Ghost platform
    bool ghost = false;
    bool visible = true;
    float timer = 0.0f;
    float visibleTime = 2.0f;
    float hiddenTime = 2.0f;
};

struct Switch { Vector3 pos; bool on; int groupID; Color col; };

struct Checkpoint     { Vector3 pos; bool on; };

struct Flash  { Color col; float t, dur; };

struct MovingPlat { int idx; float minX, maxX, speed, cur, prevX; int dir; bool active; };

static Wave MakeSweep(float f0, float f1, float dur, float vol, float atk = 0.01f)
{
    int sr = 44100, n = (int)(sr * dur);
    Wave w; w.frameCount = n; w.sampleRate = sr; w.sampleSize = 16; w.channels = 1;
    short* d = (short*)MemAlloc(n * sizeof(short)); w.data = d;
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / sr, frac = t / dur, freq = f0 + (f1 - f0) * frac;
        ph += freq / sr;

        float env = 1;
        if (t < atk) env = t / atk;
        float rs = dur * .5f;
        if (t > rs) env *= 1 - (t - rs) / (dur - rs);
        d[i] = (short)(sinf(ph * 6.2831853f) * env * vol * 32767);
    }
    return w;
}

static Wave MakeTone(float freq, float dur, float vol, int sh, float atk = 0.01f, float rel = 0.15f)
{
    int sr = 44100, n = (int)(sr * dur);
    Wave w; w.frameCount = n; w.sampleRate = sr; w.sampleSize = 16; w.channels = 1;
    short* d = (short*)MemAlloc(n * sizeof(short)); w.data = d;
    for (int i = 0; i < n; i++) {
        float t = (float)i / sr, env = 1;

        if (t < atk) env = t / atk;
        float rs = dur - rel;
        if (t > rs) env *= 1 - (t - rs) / rel;

        float ph = fmodf(t * freq, 1.f), s = 0;
        if      (sh == 0) s = sinf(t * freq * 6.2831853f);
        else if (sh == 1) s = (ph < .5f) ? 1.f : -1.f;
        else if (sh == 2) s = (ph < .5f) ? (4*ph-1) : (3-4*ph);
        else              s = (float)GetRandomValue(-1000, 1000) / 1000.f;
        d[i] = (short)(s * env * vol * 32767);
    }
    return w;
}

struct Hit { bool collided; Vector3 normal; float depth; };

Hit BoxHit(const Player& p, const Platform& pl)
{
    Hit h = { false, { 0, 1, 0 }, 0 };

    if (p.shape == ShapeType::Sphere)
    {

        float cx = Clamp(p.pos.x, pl.pos.x - pl.size.x*.5f, pl.pos.x + pl.size.x*.5f);
        float cy = Clamp(p.pos.y, pl.pos.y - pl.size.y*.5f, pl.pos.y + pl.size.y*.5f);
        float cz = Clamp(p.pos.z, pl.pos.z - pl.size.z*.5f, pl.pos.z + pl.size.z*.5f);
        float dx = p.pos.x - cx, dy = p.pos.y - cy, dz = p.pos.z - cz;
        float distSq  = dx*dx + dy*dy + dz*dz;
        if (distSq  <= p.radius * p.radius) {
            h.collided = true;
            float dist = sqrtf(distSq );
            if (dist > 0.001f) {

                h.normal = Vector3Scale({ dx, dy, dz }, 1.f / dist);
                h.depth = p.radius - dist;
            } else {

                h.normal = { 0, 1, 0 };
                h.depth = p.radius;
            }
        }
    }
    else
    {

        float overlapX  = (p.size.x*.5f + pl.size.x*.5f) - fabsf(p.pos.x - pl.pos.x);
        float overlapY  = (p.size.y*.5f + pl.size.y*.5f) - fabsf(p.pos.y - pl.pos.y);
        if (overlapX  > 0 && overlapY  > 0) {
            h.collided = true;

            if (overlapY ) {
                h.normal = (p.pos.y < pl.pos.y) ? Vector3{0,-1,0} : Vector3{0,1,0};
                h.depth = overlapY ;
            } else {
                h.normal = (p.pos.x < pl.pos.x) ? Vector3{-1,0,0} : Vector3{1,0,0};
                h.depth = overlapX ;
            }
        }
    }
    return h;
}

void Resolve(Player& p, const Hit& h)
{
    if (!h.collided || h.depth < 0.001f) return;

    if (h.normal.y < -0.7f) {
        if (p.vel.y > 0) p.vel.y = 0;
        p.pos.y -= h.depth;
        return;
    }

    p.pos = Vector3Add(p.pos, Vector3Scale(h.normal, h.depth + 0.005f));
    float vd = Vector3DotProduct(p.vel, h.normal);
    if (vd < 0) p.vel = Vector3Subtract(p.vel, Vector3Scale(h.normal, vd));
}

static std::vector<Particle> gParts;

void UpdateParts(float dt)
{
    for (auto& p : gParts) {
        p.pos  = Vector3Add(p.pos, Vector3Scale(p.vel, dt));
        p.vel.y -= 9 * dt;
        p.life  -= dt;

        p.col.a  = (unsigned char)(255 * Clamp(p.life / p.maxLife, 0.f, 1.f));
    }
    gParts.erase(
        std::remove_if(gParts.begin(), gParts.end(),
                       [](const Particle& p) { return p.life <= 0; }),
        gParts.end());
}

void DrawParts()
{
    for (auto& p : gParts)
        DrawSphere(p.pos, p.size * Clamp(p.life / p.maxLife, 0.3f, 1.f), p.col);
}

static Color MGrass()   { return { 88,  198, 78,  255 }; }
static Color MDirt()    { return { 178, 112, 48,  255 }; }
static Color MWood()    { return { 188, 138, 68,  255 }; }
static Color MDkWood()  { return { 148, 98,  38,  255 }; }
static Color MSand()    { return { 228, 205, 128, 255 }; }
static Color MDkSand()  { return { 188, 162, 88,  255 }; }
static Color MCubeCol() { return { 228, 88,  48,  255 }; }
static Color MSphCol()  { return { 48,  158, 238, 255 }; }
static Color MSWon()    { return { 68,  228, 118, 255 }; }
static Color MSWoff()   { return { 188, 48,  48,  255 }; }

static bool PlatActive(const Platform& pl, ShapeType s)
{
    if (!pl.solid)                                         return false;
    if (pl.ghost && !pl.visible)                           return false;

    if (pl.modeVis == 1 && s != ShapeType::Cube)           return false;
    if (pl.modeVis == 2 && s != ShapeType::Sphere)         return false;

    return true;
}

static void DrawPlatform(const Platform& p, ShapeType cur)
{

    if (p.modeVis == 1 && cur != ShapeType::Cube)   return;
    if (p.modeVis == 2 && cur != ShapeType::Sphere) return;
    if (!p.solid) return;
    if(!p.visible&& p.ghost) return;

    //Flickering animatie
    if(p.ghost&& p.visible)
    {
            float timeleft = p.visibleTime - p.timer;
            if(timeleft <0.8f)
            {
                bool flickeroff =((int)(p.timer*14.0f)%2 ==0);
                if(flickeroff)
                {
                    return;
                }
            }
    }

    Color top  = p.top,  side = p.side;
    if (p.modeVis == 1) { top = MCubeCol(); side = ColorBrightness(MCubeCol(), -0.35f); }
    if (p.modeVis == 2) { top = MSphCol();  side = ColorBrightness(MSphCol(),  -0.35f); }

    if (p.ghost)
    {
    top.a = 150;
    side.a = 110;
    }
    
    DrawCube(p.pos, p.size.x, p.size.y, p.size.z, side);

    float capH = 0.28f;
    Vector3 tp = { p.pos.x, p.pos.y + p.size.y*.5f + capH*.5f, p.pos.z };
    DrawCube(tp, p.size.x, capH, p.size.z, top);

    float faceZ = p.pos.z + p.size.z*.5f;
    DrawCube({ p.pos.x, p.pos.y, faceZ+0.04f }, p.size.x, p.size.y, 0.08f,
             ColorBrightness(side, 0.1f));

    Vector3 bp = { p.pos.x, p.pos.y - p.size.y*.5f + 0.06f, p.pos.z };
    DrawCube(bp, p.size.x, 0.12f, p.size.z, ColorBrightness(side, -0.35f));

    DrawCubeWires(p.pos, p.size.x+.02f, p.size.y+.02f, p.size.z+.02f, {0,0,0,30});
}

static void DrawGhost(const Platform& p, ShapeType cur)
{
    if (!p.solid) return;
    Color wc = { 0, 0, 0, 0 };
    if      (p.modeVis == 1 && cur != ShapeType::Cube)   wc = { 228, 88,  48, 90 };
    else if (p.modeVis == 2 && cur != ShapeType::Sphere) wc = { 48,  158, 238, 90 };
    else return;
    DrawCubeWires(p.pos, p.size.x, p.size.y, p.size.z, wc);
}

static void DrawSwitch(const Switch& sw, float time)
{
    float   bob = sinf(time * 3 + sw.pos.x) * .12f;
    Vector3 p   = { sw.pos.x, sw.pos.y + bob, sw.pos.z };
    Color   c   = sw.on ? MSWon() : MSWoff();

    DrawCube(p, .75f, .75f, .75f, c);

    DrawCube({ p.x, p.y, p.z + .38f }, .46f, .46f, .05f, ColorBrightness(c, 0.4f));
    DrawCubeWires(p, .77f, .77f, .77f, BLACK);

    float pulse = 0.5f + 0.5f * sinf(time * 4);
    DrawCubeWires(p,
        .94f + pulse*.13f, .94f + pulse*.13f, .94f + pulse*.13f,
        { c.r, c.g, c.b, (unsigned char)(45 + 30 * pulse) });
}

static void DrawHill(float closestX, float closestY, float closestZ, float r, Color c)
{
    DrawSphere({ closestX,          closestY,              closestZ }, r,          c);
    DrawSphere({ closestX - r*.55f, closestY - r*.25f,     closestZ }, r * .65f,  ColorBrightness(c, -0.05f));
    DrawSphere({ closestX + r*.6f,  closestY - r*.22f,     closestZ }, r * .60f,  ColorBrightness(c, -0.08f));
}

static void DrawFlagpole(const Checkpoint & cp)
{
    bool on = cp.on;
    DrawCylinder({ cp.pos.x, cp.pos.y-.6f, cp.pos.z }, 0.09f, 0.09f, 4.2f, 8, { 238,238,238,255 });
    DrawSphere  ({ cp.pos.x, cp.pos.y+3.6f, cp.pos.z }, 0.22f, on ? GOLD : GRAY);
    Color fc = on ? Color{255,72,48,255} : Color{140,140,140,200};
    DrawCube({ cp.pos.x+.38f, cp.pos.y+2.8f, cp.pos.z }, .60f, .55f, .06f, fc);
    if (on) DrawCube({ cp.pos.x+.38f, cp.pos.y+2.8f, cp.pos.z+.04f }, .22f, .22f, .06f, YELLOW);
    DrawCylinder({ cp.pos.x, cp.pos.y-.65f, cp.pos.z }, 0.42f, 0.35f, 0.18f, 8, { 165,165,165,230 });
}

static void DrawMtnBlock(float cx, float cy, float cz, float w, float h, Color c)
{
    DrawCube({ cx, cy+h*.28f, cz }, w,       h*.55f, w*.45f, ColorBrightness(c, -0.18f));
    DrawCube({ cx, cy+h*.62f, cz }, w*.65f,  h*.40f, w*.32f, c);
    DrawCube({ cx, cy+h*.88f, cz }, w*.36f,  h*.30f, w*.18f, ColorBrightness(c, 0.12f));
    DrawCube({ cx, cy+h,      cz }, w*.20f,  h*.18f, w*.10f, { 245,248,252,255 });
}

static void DrawCloud(float cx, float cy, float cz, float s, unsigned char a)
{
    Color cc = { 255,255,255,a }, cs = { 235,242,255,a };
    DrawSphere({ cx,            cy,            cz }, s,          cc);
    DrawSphere({ cx + s*.72f,  cy - s*.1f,    cz }, s*.70f,     cs);
    DrawSphere({ cx - s*.68f,  cy - s*.12f,   cz }, s*.66f,     cs);
    DrawSphere({ cx + s*.15f,  cy + s*.52f,   cz }, s*.50f,     cc);
    DrawSphere({ cx - s*.28f,  cy + s*.44f,   cz }, s*.42f,     cs);
}

static void DrawMenuScreen(int sw, int sh, float time)
{
    DrawRectangleGradientV(0, 0, sw, sh, { 60,140,220,255 }, { 108,198,248,255 });

    for (int i = 0; i < 5; i++) {
        float drift = fmodf(time * 18.f + i * 180.f, sw + 240.f) - 120.f;
        float cy2   = (float)(sh / 5 + i * 28);
        float sz    = 38.f + i * 8.f;
        unsigned char a = (unsigned char)(170 + i * 12);
        DrawEllipse((int)drift,             (int)cy2,      (int)sz,       (int)(sz*.45f), { 255,255,255,a });
        DrawEllipse((int)(drift - sz*.6f),  (int)(cy2+8),  (int)(sz*.7f), (int)(sz*.38f), { 255,255,255,(unsigned char)(a-30) });
        DrawEllipse((int)(drift + sz*.55f), (int)(cy2+6),  (int)(sz*.65f),(int)(sz*.36f), { 255,255,255,(unsigned char)(a-40) });
    }

    const char* title = "ORBICUBE";
    int tfs = 86;
    int tw2 = MeasureText(title, tfs);
    int tx  = sw/2 - tw2/2, ty = sh/4;
    DrawText(title, tx+4, ty+5, tfs, { 0,0,0,80 });
    float pulse    = 0.5f + 0.5f * sinf(time * 2.2f);
    Color titleCol = {
        (unsigned char)(228 + pulse*18),
        (unsigned char)( 88 + pulse*30),
        (unsigned char)( 48 + pulse*10), 255
    };
    DrawText(title, tx, ty, tfs, titleCol);

    const char* sub = "A 2D / 3D Platformer";
    int sw3 = MeasureText(sub, 28);
    DrawText(sub, sw/2 - sw3/2, ty + tfs + 12, 28, { 240,240,255,210 });

    int pillY = ty + tfs + 70, pillW = 220, pillH = 52, gap = 24;
    int pillX = sw/2 - (pillW*2+gap)/2;
    DrawRectangleRounded({ (float)pillX,(float)pillY,(float)pillW,(float)pillH }, .4f, 8, MCubeCol());
    DrawRectangleRoundedLines({ (float)pillX,(float)pillY,(float)pillW,(float)pillH }, .4f, 8, { 0,0,0,60 });
    { int lw2 = MeasureText("2D  CUBE", 22); DrawText("2D  CUBE", pillX+pillW/2-lw2/2, pillY+15, 22, WHITE); }

    int sphX = pillX + pillW + gap;
    DrawRectangleRounded({ (float)sphX,(float)pillY,(float)pillW,(float)pillH }, .4f, 8, MSphCol());
    DrawRectangleRoundedLines({ (float)sphX,(float)pillY,(float)pillW,(float)pillH }, .4f, 8, { 0,0,0,60 });
    { int lw2 = MeasureText("3D  SPHERE", 22); DrawText("3D  SPHERE", sphX+pillW/2-lw2/2, pillY+15, 22, WHITE); }

    int boxW = 480, boxH = 160;
    int boxX = sw/2 - boxW/2, boxY = pillY + pillH + 36;
    DrawRectangleRounded({ (float)boxX,(float)boxY,(float)boxW,(float)boxH }, .22f, 8, { 0,0,0,145 });
    DrawRectangleRoundedLines({ (float)boxX,(float)boxY,(float)boxW,(float)boxH }, .22f, 8, { 255,255,255,40 });
    int lx2 = boxX+22, ly2 = boxY+16, ls = 20, lg = 28;
    DrawRectangle(lx2, ly2, 14, 14, MCubeCol()); DrawText("A / D - move   SPACE - jump",  lx2+22, ly2, ls, RAYWHITE); ly2 += lg;
    DrawRectangle(lx2, ly2, 14, 14, MSphCol());  DrawText("WASD - move ",                 lx2+22, ly2, ls, RAYWHITE); ly2 += lg;
    DrawRectangle(lx2, ly2, 14, 14, MSWon());    DrawText("[E] - activate switch",         lx2+22, ly2, ls, RAYWHITE); ly2 += lg;
    DrawText("[1] Cube   [2] Sphere   [R] Respawn   [TAB] Fullscreen", lx2, ly2, ls, { 200,200,200,200 });

    float blink = 0.55f + 0.45f * sinf(time * 3.8f);
    unsigned char ba = (unsigned char)(200 * blink);
    const char* ps  = "PRESS  SPACE  TO  PLAY";
    int psw2 = MeasureText(ps, 38);
    int psy  = boxY + boxH + 44;
    DrawText(ps, sw/2 - psw2/2 + 3, psy+3, 38, { 0,0,0,(unsigned char)(ba/3) });
    DrawText(ps, sw/2 - psw2/2, psy, 38, { 255,230,80,ba });

    DrawText("v5", sw-52, sh-28, 18, { 255,255,255,80 });
}

static void DrawWinScreen(int sw, int sh, float time)
{

    DrawRectangle(0, 0, sw, sh, { 0, 0, 0, 175 });

    for (int i = 0; i < 40; i++) {
        float seed = (float)i * 137.508f;
        float x = fmodf(seed*3.7f + time*22*(1+(i%3)*.4f), (float)sw);
        float y = fmodf(seed*5.1f + time*48*(1+(i%5)*.2f), (float)sh);
        Color cc = {
            (unsigned char)(128 + (i*37)%127),
            (unsigned char)( 80 + (i*61)%175),
            (unsigned char)( 50 + (i*83)%205), 200
        };
        DrawRectanglePro({ x, y, 10.f+(float)(i%3)*5.f, (float)(4+i%4) }, { 5.f,2.f },
                         seed + time*20, cc);
    }

    const char* title = "LEVEL  COMPLETE!";
    int tfs = 80;
    int tw2 = MeasureText(title, tfs);
    int ty  = sh/4;
    DrawText(title, sw/2 - tw2/2 + 5, ty+6, tfs, { 0,0,0,90 });

    float sh2 = sinf(time * 4) * 6;
    DrawText(title, sw/2 - tw2/2 + (int)sh2, ty, tfs, { 255,210,0,255 });

    float blink = 0.55f + 0.45f * sinf(time * 3.2f);
    unsigned char ba = (unsigned char)(210 * blink);
    const char* rp  = "PRESS  R  TO  PLAY  AGAIN";
    int rpw = MeasureText(rp, 34);
    int rpy = sh/4 + tfs + 84 + 100 + 90 + 42;
    DrawText(rp, sw/2 - rpw/2, rpy, 34, { 255,230,80,ba });

    const char* eq  = "Press  ESC  to  quit";
    int eqw = MeasureText(eq, 20);
    DrawText(eq, sw/2 - eqw/2, rpy+52, 20, { 200,200,200,160 });
}

int main()
{

    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(1920, 1080, "ORBICUBE");
    SetTargetFPS(180);
    DisableCursor();
    InitAudioDevice();
    ToggleFullscreen();

    Wave wJ  = MakeSweep(300, 600,  .10f, .42f, .003f); Sound sndJump    = LoadSoundFromWave(wJ);  UnloadWave(wJ);
    Wave wL  = MakeSweep(160,  52,  .11f, .50f, .002f); Sound sndLand    = LoadSoundFromWave(wL);  UnloadWave(wL);
    Wave wM  = MakeSweep(440, 880,  .16f, .35f, .008f); Sound sndMorph    = LoadSoundFromWave(wM);  UnloadWave(wM);
    Wave wSw = MakeTone (940,        .08f, .38f, 1, .002f, .05f); Sound sndSwitch  = LoadSoundFromWave(wSw); UnloadWave(wSw);
    Wave wS1 = MakeSweep(523, 784,  .28f, .38f, .008f); Sound sS1  = LoadSoundFromWave(wS1); UnloadWave(wS1);
    Wave wS2 = MakeSweep(659, 988,  .28f, .28f, .016f); Sound sS2  = LoadSoundFromWave(wS2); UnloadWave(wS2);
    Wave wCP = MakeTone (660,        .22f, .32f, 0, .008f, .18f); Sound sCP = LoadSoundFromWave(wCP); UnloadWave(wCP);
    Wave wSt = MakeTone (220,        .04f, .10f, 3, .001f, .02f); Sound sSt = LoadSoundFromWave(wSt); UnloadWave(wSt);
    Wave wW1 = MakeSweep(440,  1320, .55f, .45f, .012f); Sound sWin1 = LoadSoundFromWave(wW1); UnloadWave(wW1);
    Wave wW2 = MakeSweep(660,  1760, .55f, .32f, .025f); Sound sWin2 = LoadSoundFromWave(wW2); UnloadWave(wW2);

    float stepT = 0;

    const float THICKNESS   = 2.f;
    const float GROUND  = 0;
    const float HEIGHT_MID  = 3.f;
    const float HEIGHT_HIGH = 6.f;

    auto PY = [](float s) { return s - 1.f; };

    std::vector<Platform> world;

    auto AddPlat  = [&](float x, float y, float w, float d, float z, Color top, Color side) {
        Platform pl = { {x, PY(y), z}, {w, THICKNESS, d}, top, side, 0, true };
        pl.origZ = z;
        world.push_back(pl);
    };
    auto AddCubePlat  = [&](float x, float y, float w, float d) {
        Platform pl = { {x, PY(y), 0}, {w, THICKNESS, d}, MCubeCol(), ColorBrightness(MCubeCol(), -.3f), 1, true };
        pl.origZ = 0;
        world.push_back(pl);
    };
    auto AddSpherePlat = [&](float x, float y, float w, float d, float z) {
        Platform pl = { {x, PY(y), z}, {w, THICKNESS, d}, MSphCol(), ColorBrightness(MSphCol(), -.3f), 2, true };
        pl.origZ = z;
        world.push_back(pl);
    };
    auto AddGhostPlat = [&](float x, float y, float w, float d, float z, float visibleTime,float hiddentime, float timer) {
        Platform pl = { {x,PY(y),z},{w,THICKNESS,d}, {170, 220, 255, 150}, {80, 120, 180, 120}, 0, true };

        pl.origZ = z;
        pl.ghost =true;
        pl.visible = true;
        pl.timer = 0.0f;
        pl.visibleTime = visibleTime;
        pl.hiddenTime = hiddentime;
        pl.timer =  timer;
        
        world.push_back(pl);
    };

    // Plaatsen van de platforms
    AddPlat(  0, GROUND,  16, 16,  0, MGrass(), MDirt());
    AddPlat( -8, GROUND,   6, 16,  0, MGrass(), MDirt());
    AddPlat( 30, GROUND,  10, 12,  0, MGrass(), MDirt());
    AddPlat( 44, HEIGHT_HIGH,  8, 12,  0, MGrass(), MDirt());
    AddPlat( 56, GROUND,  10, 14,  0, MGrass(), MDirt());
    AddCubePlat(13, GROUND,   4,  6);
    AddSpherePlat(34, HEIGHT_MID,  8,  6,  8);

    int bridgeIdx = (int)world.size();
    Platform bridge = { {66, PY(GROUND), 0}, {9, THICKNESS, 10}, MSand(), MDkSand(), 0, false };
    bridge.origZ = 0;
    world.push_back(bridge);
    AddPlat(89, GROUND,  8, 11, 25, MGrass(), MDirt());
    
    


    AddPlat(105, GROUND,  10, 13,   0, MGrass(), MDirt());
    AddGhostPlat(119, HEIGHT_MID,  8, 10,   0, 2.0f, 2.0f,1.2f);

    AddGhostPlat(143, HEIGHT_MID, 8, 10, 0, 2.0f, 2.0f,0.5f);
    AddPlat(155, GROUND, 12, 14, 0, MWood(),  MDkWood());

    AddSpherePlat(131, HEIGHT_MID, 8,  6, -11);

    std::vector<Switch> switches;
    switches.push_back({ {57, PY(GROUND) + THICKNESS/2 + 0.85f, 0}, false, 10, {230,210,50,255} });

    std::vector<MovingPlat> movPlats;
    movPlats.push_back({ bridgeIdx, 67.f, 80.f, 7.f, 62.f, 62.f, 1, false });

    std::vector<Checkpoint> checkpoints;
    checkpoints .push_back({ {  0, 1, 0}, true  });
    checkpoints .push_back({ { 55, 1, 0}, false });
    checkpoints .push_back({ {105, 1, 0}, false });
    checkpoints .push_back({ {155, 1, 0}, false });
    int activeCheckpoint  = 2;
    const int LAST_CP = (int)checkpoints.size() - 1;

    struct HillObj  { float x, y, z, r; };
    struct CloudObj { float cx, cy, cz, s, phase; };
    struct MtnObj   { float cx, cy, cz, w, h; };

    std::vector<HillObj> hills = {
        {5,-2,-18,5.f},{28,-2,-20,6.5f},{62,-2,-18,5.5f},{98,-2,-20,5.f},{138,-2,-18,6.f},
    };
    std::vector<CloudObj> clouds = {
        {8,18,-16,3.4f,0.0f},{38,20,-14,3.0f,1.2f},{75,22,-18,3.8f,0.7f},
        {115,19,-16,3.2f,2.1f},{152,21,-14,3.6f,0.4f},{-5,20,-20,2.8f,1.8f},
    };
    std::vector<MtnObj> mountains = {
        {-10,0,-55,25,20},{22,0,-58,22,17},{58,0,-60,28,23},
        {98,0,-56,24,20},{140,0,-58,28,22},{175,0,-55,22,18},
    };

    Camera3D cam = {};
    cam.fovy = 60; cam.up = { 0,1,0 }; cam.projection = CAMERA_PERSPECTIVE;

    Player player; player.pos =  checkpoints[activeCheckpoint].pos;
    player.pos.y = 3;
    // Einde van setup

    float yaw   = 0;
    float pitch = 0.30f;
    float time  = 0;

    std::string hint;
    float hintT = 0;

    float shakeT = 0;

    int   curRoom  = 0, lastRoom = -1;
    float roomFadeT = 0;

    std::vector<Flash> flashes;

    float modePillT = 0;

    Vector3 camPos = { 0, 7, 24 }, camTgt = { 0, 2, 0 };
    float   camFov = 60;

    bool wasOnGround = false;

    Color skyTgts[3] = {
        { 108, 198, 248, 255 },
        { 218, 188, 128, 255 },
        {  88, 158,  88, 255 },
    };
    Color curSky = skyTgts[0];

    const char* roomNames[] = { "Grassland", "Desert Ruins", "Forest" };

    GameState state   = GameState::Menu;
    float     winTime = 0.f;

    auto ResetGame = [&]()
    {
        player       = Player();
        player.pos   = { 0, 3, 0 };
        activeCheckpoint     = 2;

        for (auto& cp : checkpoints ) cp.on = false;
        checkpoints [0].on = true;

        for (auto& sw : switches) sw.on = false;
        for (auto& movPlat  : movPlats) { movPlat.active = false; movPlat.cur = movPlat.minX; movPlat.dir = 1; }
        if (bridgeIdx >= 0 && bridgeIdx < (int)world.size())
            world[bridgeIdx].solid = false;

        gParts.clear();
        flashes.clear();
        hint = ""; hintT = 0;
        shakeT = 0; roomFadeT = 0;
        curRoom = 0; lastRoom = -1;
        curSky  = skyTgts[0];
        yaw = 0; pitch = 0.30f;
        camPos = { 0, 7, 24 }; camTgt = { 0, 2, 0 }; camFov = 60;
        modePillT = 0; winTime = 0;
    };
    
    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();
        if (dt > 0.05f) dt = 0.05f;
        time += dt;

        int scrW = GetScreenWidth(), scrH = GetScreenHeight();

        if (state == GameState::Menu)
        {
            if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER))
            {
                ResetGame();
                state = GameState::Playing;
            }
            BeginDrawing();
            ClearBackground({ 108, 198, 248, 255 });
            DrawMenuScreen(scrW, scrH, time);
            EndDrawing();
            continue;
        }

        if (state == GameState::Win)
        {
            winTime += dt;
            if (IsKeyPressed(KEY_R)) { ResetGame(); state = GameState::Playing; }

            BeginDrawing();
            ClearBackground({ 108, 198, 248, 255 });
            BeginMode3D(cam);
            rlEnableDepthTest(); rlEnableDepthMask();
            rlDisableBackfaceCulling();
            DrawSphere({ 135,0,0 }, 700, { curSky.r,curSky.g,(unsigned char)(curSky.b+10),255 });
            rlEnableBackfaceCulling();
            DrawPlane({ 135,-1.32f,0 }, { 580,140 }, { 82,195,65,255 });
            for (auto& hill  : hills)  DrawHill(hill .x, hill .y, hill .z, hill .r, { 75,195,62,255 });
            for (auto& m  : mountains)   DrawMtnBlock(m.cx, m.cy, m.cz, m.w, m.h, { 138,142,148,255 });
            for (auto& cloud  : clouds) { float drift = sinf(time*.10f + cloud .phase)*3.f; DrawCloud(cloud .cx+drift, cloud .cy, cloud .cz, cloud .s, 212); }
            for (auto& p  : world)  DrawPlatform(p, player.shape);
            for (auto& sw : switches) DrawSwitch(sw, time);
            for (auto& cp : checkpoints )    DrawFlagpole(cp);
            DrawParts();
            player.Draw();
            EndMode3D();
            DrawWinScreen(scrW, scrH, winTime);
            EndDrawing();
            continue;
        }

        shakeT = Clamp(shakeT - dt, 0, 10);
        hintT  = Clamp(hintT  - dt, 0, 10);

        Vector2 md = GetMouseDelta();
        yaw   += md.x * .003f;
        pitch  = Clamp(pitch - md.y * .003f, -0.95f, 0.95f);

        float   rawX = (float)(IsKeyDown(KEY_D) - IsKeyDown(KEY_A));
        Vector2 inp  = {
            (float)(IsKeyDown(KEY_A) - IsKeyDown(KEY_D)),
            (float)(IsKeyDown(KEY_S) - IsKeyDown(KEY_W))
        };
        if (Vector2Length(inp) > 1) inp = Vector2Normalize(inp);

        Vector3 sphDir = {
             inp.x * cosf(yaw) - inp.y * sinf(yaw), 0,
            -inp.x * sinf(yaw) - inp.y * cosf(yaw)
        };
        if (Vector3Length(sphDir) > 0.01f) sphDir = Vector3Normalize(sphDir);

        bool jumped = false, switched = false;
        wasOnGround = player.onGround;
        player.HandleInput(dt, rawX, sphDir, jumped, switched);
        if (jumped)   PlaySound(sndJump);
        if (switched) PlaySound(sndMorph);

        if (player.shape == ShapeType::Cube) { player.pos.z = 0; player.vel.z = 0; }

        player.Update(dt);
        UpdateParts(dt);

        for (auto& pl : world)
        {
            pl.pos.z = Lerp(pl.pos.z, (player.shape == ShapeType::Cube) ? 0.f : pl.origZ, 9.f * dt);

            if (pl.ghost)
            {                 
                pl.timer += dt;
                if (pl.visible)
                {
                    if (pl.timer >= pl.visibleTime)
                    {
                        pl.visible = false;
                        pl.timer = 0.0f;
                    }
                }
                else
                {
                    if (pl.timer >= pl.hiddenTime)
                    {
                        pl.visible = true;
                        pl.timer = 0.0f;
                    }
                }
            }
        }
    
        for (auto& movPlat  : movPlats)
        {
            movPlat.prevX = movPlat .cur;
            if (!movPlat.active) continue;

            movPlat.cur += movPlat .dir * movPlat.speed * dt;

            if (movPlat .cur >= movPlat .maxX) { movPlat .cur = movPlat .maxX; movPlat.dir = -1; }
            if (movPlat .cur <= movPlat .minX) { movPlat .cur = movPlat .minX; movPlat.dir =  1; }

            if (movPlat.idx >= 0 && movPlat.idx < (int)world.size())
                world[movPlat.idx].pos.x = movPlat.cur;
        }

        float preVelY = player.vel.y;
        for (auto& pl : world)
        {
            if (!PlatActive(pl, player.shape)) continue;

            Hit h = BoxHit(player, pl);
            if (!h.collided) continue;

            bool isFloor = (h.normal.y > 0.7f && preVelY <= 0);
            Resolve(player, h);

            if (isFloor) player.OnLand(pl.pos.y + pl.size.y*.5f, fabsf(preVelY));
        }

        for (auto& movPlat  : movPlats)
        {
            if (!movPlat.active || !player.onGround) continue;
            float delta = movPlat.cur - movPlat.prevX;
            if (fabsf(delta) < 0.0001f) continue;

            auto& pl    = world[movPlat.idx];
            float halfW = pl.size.x*.5f +
                (player.shape == ShapeType::Sphere ? player.radius : player.size.x*.5f);
            float dx = fabsf(player.pos.x - pl.pos.x);
            float dy = player.pos.y - (pl.pos.y + pl.size.y*.5f);

            if (dx < halfW && dy > -0.15f && dy < 1.8f)
                player.pos.x += delta;
        }

        if (!wasOnGround && player.onGround)
        {
            float impact = fabsf(preVelY);
            if (impact > 1.5f) {

                SetSoundPitch(sndLand, 0.82f + Clamp(impact / 22.f, 0.f, 0.55f));
                PlaySound(sndLand );
                player.SpawnLandParticles(gParts, impact);
            }
        }

        if (player.shape == ShapeType::Cube && player.onGround && fabsf(player.vel.x) > 1.2f)
        {
            stepT -= dt;
            if (stepT <= 0) {
                float sr = fabsf(player.vel.x) / 9.f;
                SetSoundPitch(sSt, 0.78f + sr * 0.45f);
                PlaySound(sSt);
                stepT = 0.26f - sr * 0.07f;
            }
        }
        else stepT = 0;

        if (player.pos.y < checkpoints [activeCheckpoint].pos.y - 12 || IsKeyPressed(KEY_R))
        {
            player.pos = checkpoints [activeCheckpoint].pos;
            player.vel = { 0, 0, 0 };
            player.onGround = false;
            flashes.push_back({ RED, 0.28f, 0.28f });
        }

        for (int i = 0; i < (int)checkpoints .size(); i++)
        {
            if (checkpoints [i].on) continue;

            if (Vector3LengthSqr(Vector3Subtract(player.pos, checkpoints[i].pos)) < 13)
            {
                checkpoints [i].on = true;
                if (i > activeCheckpoint) activeCheckpoint = i;

                if (i == LAST_CP)
                {

                    flashes.push_back({ GOLD, 1.2f, 1.2f });
                    player.SpawnCollectParticles(gParts, GOLD);
                    PlaySound(sWin1); PlaySound(sWin2);
                    state   = GameState::Win;
                    winTime = 0.f;
                }
                else
                {

                    hint = "Checkpoint!"; hintT = 2.5f;
                    flashes.push_back({ {72,255,138,255}, 0.30f, 0.30f });
                    PlaySound(sCP);
                }
            }
        }

        if (IsKeyPressed(KEY_E))
        {
            for (auto& sw : switches)
            {

                if (Vector3LengthSqr(Vector3Subtract(player.pos, sw.pos)) > 11.f) continue;

                sw.on = !sw.on;

                for (auto& movPlat  : movPlats)
                    if (movPlat.idx == bridgeIdx) { movPlat.active = sw.on; world[movPlat.idx].solid = sw.on; }

                hint  = sw.on ? "Switch ON" : "Switch OFF";
                hintT = 3; shakeT = 0.2f;
                Color fc = sw.on ? MSWon() : MSWoff();
                flashes.push_back({ fc, 0.32f, 0.32f });
                PlaySound(sndSwitch);
                player.SpawnCollectParticles(gParts, fc);
            }
        }

        modePillT = Lerp(modePillT, (player.shape == ShapeType::Sphere) ? 1.f : 0.f, 12*dt);

        float px2 = player.pos.x;
        if      (px2 < 52)  curRoom = 0;
        else if (px2 < 105) curRoom = 1;
        else                curRoom = 2;

        if (curRoom != lastRoom) { roomFadeT = 1.4f; lastRoom = curRoom; }
        if (roomFadeT > 0) roomFadeT -= dt;

        Color ts = skyTgts[curRoom];
        curSky.r = (unsigned char)Lerp((float)curSky.r, (float)ts.r, 1.8f*dt);
        curSky.g = (unsigned char)Lerp((float)curSky.g, (float)ts.g, 1.8f*dt);
        curSky.b = (unsigned char)Lerp((float)curSky.b, (float)ts.b, 1.8f*dt);

        if (player.shape == ShapeType::Cube)
        {

            float lax = player.vel.x * 0.26f;
            camPos = Vector3Lerp(camPos, { player.pos.x + lax, player.pos.y + 5.5f, 27.f }, 8.f*dt);
            camTgt = Vector3Lerp(camTgt, { player.pos.x + lax*.3f, player.pos.y + 1.2f, 0.f }, 8.f*dt);
            cam.position   = camPos; cam.target = camTgt; cam.up = { 0, 1, 0 };
            cam.projection = CAMERA_ORTHOGRAPHIC; cam.fovy = 14.f;
        }
        else
        {

            Vector3 shake = {};
            if (shakeT > 0)
                shake = { (float)GetRandomValue(-2,2)*.016f, (float)GetRandomValue(-1,1)*.010f, 0 };

            Vector3 la  = { player.vel.x*.17f, player.vel.y*.05f, player.vel.z*.17f };
            Vector3 tgt = Vector3Add(Vector3Add(player.pos, la), shake);

            Vector3 cf = { cosf(pitch)*sinf(yaw), sinf(pitch), cosf(pitch)*cosf(yaw) };

            camTgt = Vector3Lerp(camTgt, tgt, 0.12f);
            camPos = Vector3Lerp(camPos, Vector3Add(tgt, Vector3Scale(cf, -14.f)), 0.10f);

            float hs = sqrtf(player.vel.x*player.vel.x + player.vel.z*player.vel.z);
            camFov = Lerp(camFov, 60.f + hs * 0.48f, 4.f * dt);

            cam.position   = camPos; cam.target = camTgt; cam.up = { 0,1,0 };
            cam.projection = CAMERA_PERSPECTIVE; cam.fovy = camFov;
        }

        for (auto& f : flashes) f.t -= dt;
        flashes.erase(
            std::remove_if(flashes.begin(), flashes.end(), [](const Flash& f){ return f.t <= 0; }),
            flashes.end());

        BeginDrawing();
        ClearBackground(curSky);
        BeginMode3D(cam);
        rlEnableDepthTest(); rlEnableDepthMask();

        if (player.shape == ShapeType::Sphere)
        {

            rlDisableBackfaceCulling();
            DrawSphere({ 135,0,0 }, 700,
                { curSky.r, curSky.g, (unsigned char)(curSky.b+10), 255 });
            rlEnableBackfaceCulling();

            DrawCube({ 135,-2,0 }, 620, 2.5f, 110,
                { curSky.r, (unsigned char)(curSky.g+5), (unsigned char)(curSky.b+16), 255 });
            DrawPlane({ 135,-1.32f, 0 }, { 580, 140 }, { 82,195,65,255 });
            DrawPlane({ 135,-1.28f,-45 }, { 580, 70 }, { 65,178,50,255 });

            Color hillCol = { 75,195,62,255 };
            if (curRoom == 1) hillCol = { 165,148,95,255 };
            if (curRoom == 2) hillCol = { 55,138,45,255 };
            for (auto& hill  : hills) DrawHill(hill .x, hill .y, hill .z, hill .r, hillCol);

            Color mc = { 138,142,148,255 };
            if (curRoom == 1) mc = { 168,152,108,255 };
            if (curRoom == 2) mc = { 85,128,72,255 };
            for (auto& m : mountains) DrawMtnBlock(m.cx, m.cy, m.cz, m.w, m.h, mc);

            for (auto& cloud  : clouds) {
                float drift = sinf(time*.10f + cloud .phase) * 3.f;
                DrawCloud(cloud .cx + drift, cloud .cy, cloud .cz, cloud .s, 212);
            }
        }
        else
        {

            DrawPlane({ 135,-1.2f,0 }, { 480,120 }, { 90,188,70,255 });
        }

        for (auto& p : world) DrawPlatform(p, player.shape);
        for (auto& p : world) DrawGhost(p, player.shape);
        rlEnableDepthMask();
        for (auto& sw : switches) DrawSwitch(sw, time);
        for (auto& cp : checkpoints )      DrawFlagpole(cp);
        DrawParts();
        player.Draw();
        EndMode3D();

        for (auto& f : flashes) {
            float prog = f.t / f.dur;
            unsigned char fa = (unsigned char)(Clamp(prog * 2.f, 0.f, 1.f) * 65);
            DrawRectangle(0, 0, scrW, scrH, { f.col.r, f.col.g, f.col.b, fa });
        }

        for (int i = 0; i < 7; i++) {
            float  t2 = (float)i / 7;
            unsigned char a = (unsigned char)(t2 * t2 * 95);
            DrawRectangleLinesEx(
                { -(float)i*3, -(float)i*3, (float)scrW+i*6, (float)scrH+i*6 },
                (float)(i+1)*6, { 0,0,0,a });
        }

        if (roomFadeT > 0) {
            float p2  = roomFadeT / 1.4f;

            float fa2 = p2 > 0.72f ? (p2 - 0.72f) / 0.28f : 0;
            DrawRectangle(0, 0, scrW, scrH, { 0,0,0,(unsigned char)(fa2 * 140) });
        }

        DrawRectangleRounded({ (float)scrW/2 - 148, 10, 296, 47 }, .3f, 6, { 0,0,0,148 });
        {
            int tw2 = MeasureText(roomNames[curRoom], 24);
            DrawText(roomNames[curRoom], scrW/2 - tw2/2, 20, 24, RAYWHITE);
        }

        {
            float pw = 280, ph = 52, px3 = (float)scrW - pw - 10, py3 = 10;
            bool  ic = player.shape == ShapeType::Cube;
            Color cc = ic ? MCubeCol() : MSphCol();
            DrawRectangleRounded({ px3, py3, pw, ph }, .5f, 8, { 0,0,0,148 });
            float slx = px3 + 4 + modePillT * (pw/2 - 4);
            DrawRectangleRounded({ slx, py3+4, pw/2-8, ph-8 }, .5f, 8, ColorAlpha(cc, .90f));
            const char *l2 = "2D [1]", *l3 = "3D [2]";
            int lw2 = MeasureText(l2, 20), lw3 = MeasureText(l3, 20);
            DrawText(l2, (int)(px3 + pw/4 - lw2/2),   (int)(py3+16), 20, ic  ? WHITE : Color{200,200,200,130});
            DrawText(l3, (int)(px3 + 3*pw/4 - lw3/2), (int)(py3+16), 20, !ic ? WHITE : Color{200,200,200,130});
        }

        {
            bool ic = player.shape == ShapeType::Cube;
            const char* adv = ic
                ? "2D: Fast accel  Cube-only paths"
                : "3D:  Off-axis paths  Sphere routes";
            Color ac = ic ? MCubeCol() : MSphCol();
            int aw = MeasureText(adv, 16);
            DrawRectangleRounded({ 10, (float)scrH-132, (float)aw+24, 28 }, .3f, 6, { 0,0,0,145 });
            DrawText(adv, 22, scrH-128, 16, ac);
        }

        for (auto& sw : switches) {
            if (Vector3LengthSqr(Vector3Subtract(player.pos, sw.pos)) < 16) {
                const char* et  = "[E] Activate";
                int         etw = MeasureText(et, 24);
                DrawRectangleRounded(
                    { (float)(scrW/2 - etw/2 - 14), (float)(scrH/2 + 62), (float)(etw+28), 40 },
                    .4f, 6, { 0,0,0,162 });
                DrawText(et, scrW/2 - etw/2, scrH/2+70, 24, sw.on ? MSWon() : MSWoff());
            }
        }

        DrawRectangleRounded({ 10, (float)scrH-100, 378, 88 }, .25f, 6, { 0,0,0,148 });
        DrawRectangle(18, scrH-90, 14, 14, MCubeCol()); DrawText("Cube 2D: A/D move",                         36, scrH-92, 17, RAYWHITE);
        DrawRectangle(18, scrH-70, 14, 14, MSphCol());  DrawText("Sphere 3D: WASD",                            36, scrH-72, 17, RAYWHITE);
        DrawRectangle(18, scrH-50, 14, 14, MSWon());    DrawText("Switch [E]  R=respawn  TAB=fullscreen",      36, scrH-52, 17, RAYWHITE);
        DrawText("SPACE = jump (hold for higher)", scrW/2 - 165, scrH-24, 18, { 255,255,255,128 });

        if (hintT > 0) {
            float a  = Clamp(hintT * 1.5f, 0.f, 1.f);
            unsigned char ua = (unsigned char)(255 * a);
            int htw  = MeasureText(hint.c_str(), 32);
            int htx  = scrW/2 - htw/2, hty = scrH/2 - 115;
            DrawRectangle(htx-18, hty-10, htw+36, 54, { 0,0,0,(unsigned char)(148*a) });
            DrawText(hint.c_str(), htx, hty, 32, { 255,218,48,ua });
        }

        DrawFPS(scrW - 88, scrH - 26);
        
        EndDrawing();
    }

    UnloadSound(sndJump);  UnloadSound(sndLand);   UnloadSound(sndMorph);
    UnloadSound(sndSwitch); UnloadSound(sS1);  UnloadSound(sS2);
    UnloadSound(sCP); UnloadSound(sSt);  UnloadSound(sWin1); UnloadSound(sWin2);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}