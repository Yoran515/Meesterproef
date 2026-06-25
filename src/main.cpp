

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "Player.h"
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

// =============================================================================
// Game-state enum
// =============================================================================

/** Top-level state machine that controls which screen is rendered. */
enum class GameState { Menu, Playing, Win };

// =============================================================================
//   POD structs
// =============================================================================

/**
 * Platform — the fundamental collideable geometry unit.
 *
 * Platforms are axis-aligned boxes described by a centre position and a size.
 * `modeVis`   — visibility / collision filter:
 *                0 = always visible/solid
 *                1 = cube-only  (drawn orange; only collides in Cube mode)
 *                2 = sphere-only(drawn blue;   only collides in Sphere mode)
 * `solid`     — if false the platform is treated as non-existent for collision.
 *               Used to open/close the moving bridge.
 * `origZ`     — the "resting" Z position the platform lerps to in 3-D mode.
 *               In 2-D (Cube) mode all platforms lerp their Z to 0.
 */
struct Platform {
    Vector3 pos, size;
    Color   top, side;
    int     modeVis = 0;
    bool    solid   = true;
    float   origZ   = 0;
};

/**
 * Switch — interactive toggle that activates moving platforms.
 *
 * `groupID` — not currently used for grouping; reserved for multi-switch puzzles.
 * `on`      — current toggle state; flipped when the player presses [E] nearby.
 */
struct Switch { Vector3 pos; bool on; int groupID; Color col; };

/** Checkpoint flag pole.  `on` becomes true when the player enters its radius. */
struct CP     { Vector3 pos; bool on; };

/** Screen-space full-screen colour flash (e.g., on landing, checkpoint, respawn). */
struct Flash  { Color col; float t, dur; };

/**
 * MovingPlat — animates a Platform back-and-forth along the X axis.
 *
 * `idx`    — index into the world[] Platform array this mover controls.
 * `minX/maxX` — travel endpoints.
 * `speed`  — units/second of travel.
 * `cur`    — current X position of the platform.
 * `prevX`  — X from the previous frame; delta used to carry the player.
 * `dir`    — +1 or −1 (direction of current travel).
 * `active` — movement only runs when this is true (switch-controlled).
 */
struct MovingPlat { int idx; float minX, maxX, speed, cur, prevX; int dir; bool active; };

// =============================================================================
//  Procedural audio helpers
// =============================================================================
//
//  All sounds are synthesised at runtime from mathematical functions so the
//  game ships with zero audio assets.  Two helper functions cover the two
//  most common waveform shapes needed:

/**
 * MakeSweep — frequency-swept sine wave (good for whooshes and jumps).
 *
 * @param f0  Start frequency in Hz.
 * @param f1  End frequency in Hz.
 * @param dur Duration in seconds.
 * @param vol Amplitude [0, 1].
 * @param atk Attack time in seconds (linear fade-in from silence).
 *
 * The frequency transitions linearly from f0→f1 over `dur` seconds.
 * The amplitude envelope fades in over `atk` seconds and then fades out
 * over the second half of the duration.
 */
static Wave MakeSweep(float f0, float f1, float dur, float vol, float atk = 0.01f)
{
    int sr = 44100, n = (int)(sr * dur);
    Wave w; w.frameCount = n; w.sampleRate = sr; w.sampleSize = 16; w.channels = 1;
    short* d = (short*)MemAlloc(n * sizeof(short)); w.data = d;
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / sr, frac = t / dur, freq = f0 + (f1 - f0) * frac;
        ph += freq / sr;
        // Envelope: ramp up over `atk`, then fade out over the second half.
        float env = 1;
        if (t < atk) env = t / atk;
        float rs = dur * .5f;
        if (t > rs) env *= 1 - (t - rs) / (dur - rs);
        d[i] = (short)(sinf(ph * 6.2831853f) * env * vol * 32767);
    }
    return w;
}

/**
 * MakeTone — fixed-frequency wave with selectable waveform shape.
 *
 * @param freq Frequency in Hz.
 * @param dur  Duration in seconds.
 * @param vol  Amplitude [0, 1].
 * @param sh   Shape: 0=sine, 1=square, 2=triangle, 3=white noise.
 * @param atk  Attack time.
 * @param rel  Release time.
 */
static Wave MakeTone(float freq, float dur, float vol, int sh, float atk = 0.01f, float rel = 0.15f)
{
    int sr = 44100, n = (int)(sr * dur);
    Wave w; w.frameCount = n; w.sampleRate = sr; w.sampleSize = 16; w.channels = 1;
    short* d = (short*)MemAlloc(n * sizeof(short)); w.data = d;
    for (int i = 0; i < n; i++) {
        float t = (float)i / sr, env = 1;
        // Envelope: linear attack then linear release.
        if (t < atk) env = t / atk;
        float rs = dur - rel;
        if (t > rs) env *= 1 - (t - rs) / rel;
        // Phase 0-1 within each cycle.
        float ph = fmodf(t * freq, 1.f), s = 0;
        if      (sh == 0) s = sinf(t * freq * 6.2831853f);          // Sine
        else if (sh == 1) s = (ph < .5f) ? 1.f : -1.f;             // Square
        else if (sh == 2) s = (ph < .5f) ? (4*ph-1) : (3-4*ph);   // Triangle
        else              s = (float)GetRandomValue(-1000, 1000) / 1000.f; // Noise
        d[i] = (short)(s * env * vol * 32767);
    }
    return w;
}

// =============================================================================
//  Collision detection and resolution
// =============================================================================

struct Hit { bool yes; Vector3 n; float d; };


Hit BoxHit(const Player& p, const Platform& pl)
{
    Hit h = { false, { 0, 1, 0 }, 0 };

    if (p.shape == ShapeType::Sphere)
    {
        // Clamp sphere centre to the platform's AABB to find the closest point.
        float cx = Clamp(p.pos.x, pl.pos.x - pl.size.x*.5f, pl.pos.x + pl.size.x*.5f);
        float cy = Clamp(p.pos.y, pl.pos.y - pl.size.y*.5f, pl.pos.y + pl.size.y*.5f);
        float cz = Clamp(p.pos.z, pl.pos.z - pl.size.z*.5f, pl.pos.z + pl.size.z*.5f);
        float dx = p.pos.x - cx, dy = p.pos.y - cy, dz = p.pos.z - cz;
        float dsq = dx*dx + dy*dy + dz*dz;
        if (dsq <= p.radius * p.radius) {
            h.yes = true;
            float dist = sqrtf(dsq);
            if (dist > 0.001f) {
                // Normal = direction from closest point to sphere centre.
                h.n = Vector3Scale({ dx, dy, dz }, 1.f / dist);
                h.d = p.radius - dist;
            } else {
                // Sphere centre exactly on the platform surface; push up.
                h.n = { 0, 1, 0 };
                h.d = p.radius;
            }
        }
    }
    else
    {
        // AABB overlap on X and Y (Z is ignored in 2-D cube mode).
        float oX = (p.size.x*.5f + pl.size.x*.5f) - fabsf(p.pos.x - pl.pos.x);
        float oY = (p.size.y*.5f + pl.size.y*.5f) - fabsf(p.pos.y - pl.pos.y);
        if (oX > 0 && oY > 0) {
            h.yes = true;
            // Pick the axis with the smaller overlap as the push direction —
            // this correctly handles corner cases without complex logic.
            if (oY < oX) {
                h.n = (p.pos.y < pl.pos.y) ? Vector3{0,-1,0} : Vector3{0,1,0};
                h.d = oY;
            } else {
                h.n = (p.pos.x < pl.pos.x) ? Vector3{-1,0,0} : Vector3{1,0,0};
                h.d = oX;
            }
        }
    }
    return h;
}


 // Resolve — pushes the player out of a detected penetration.
 
void Resolve(Player& p, const Hit& h)
{
    if (!h.yes || h.d < 0.001f) return;

    // Ceiling: push downward, cancel upward velocity.
    if (h.n.y < -0.7f) {
        if (p.vel.y > 0) p.vel.y = 0;
        p.pos.y -= h.d;
        return;
    }

    // General case: displace along normal, then cancel the velocity component
    // pointing into the surface.
    p.pos = Vector3Add(p.pos, Vector3Scale(h.n, h.d + 0.005f));
    float vd = Vector3DotProduct(p.vel, h.n);
    if (vd < 0) p.vel = Vector3Subtract(p.vel, Vector3Scale(h.n, vd));
}


//  Particle system


static std::vector<Particle> gParts;


 // UpdateParts — integrates particle physics and removes dead particles.

void UpdateParts(float dt)
{
    for (auto& p : gParts) {
        p.pos  = Vector3Add(p.pos, Vector3Scale(p.vel, dt));
        p.vel.y -= 9 * dt;               // Gentle gravity (lighter than player gravity).
        p.life  -= dt;
        // Alpha fades proportionally to remaining life so particles disappear smoothly.
        p.col.a  = (unsigned char)(255 * Clamp(p.life / p.maxLife, 0.f, 1.f));
    }
    gParts.erase(
        std::remove_if(gParts.begin(), gParts.end(),
                       [](const Particle& p) { return p.life <= 0; }),
        gParts.end());
}

// DrawParts — draws each particle as a small sphere, shrinking as it ages. 
void DrawParts()
{
    for (auto& p : gParts)
        DrawSphere(p.pos, p.size * Clamp(p.life / p.maxLife, 0.3f, 1.f), p.col);
}


//  Colour palette helpers


static Color MGrass()   { return { 88,  198, 78,  255 }; }   ///< Bright grass green.
static Color MDirt()    { return { 178, 112, 48,  255 }; }   ///< Brown dirt (platform sides).
static Color MWood()    { return { 188, 138, 68,  255 }; }   ///< Light wood.
static Color MDkWood()  { return { 148, 98,  38,  255 }; }   ///< Dark wood (platform sides).
static Color MSand()    { return { 228, 205, 128, 255 }; }   ///< Desert sand.
static Color MDkSand()  { return { 188, 162, 88,  255 }; }   ///< Darker sand (sides).
static Color MCubeCol() { return { 228, 88,  48,  255 }; }   ///< Cube-mode highlight (orange-red).
static Color MSphCol()  { return { 48,  158, 238, 255 }; }   ///< Sphere-mode highlight (sky blue).
static Color MSWon()    { return { 68,  228, 118, 255 }; }   ///< Switch active (green).
static Color MSWoff()   { return { 188, 48,  48,  255 }; }   ///< Switch inactive (red).



//  PlatActive — returns true if this platform should collide with the player.
 
static bool PlatActive(const Platform& pl, ShapeType s)
{
    if (!pl.solid)                                          return false;
    if (pl.modeVis == 1 && s != ShapeType::Cube)           return false;
    if (pl.modeVis == 2 && s != ShapeType::Sphere)         return false;
    return true;
}


//  DrawPlatform — the main terrain block renderer


static void DrawPlatform(const Platform& p, ShapeType cur)
{
    // Respect mode-visibility filter: skip platforms that are hidden for the
    // current shape, and never draw non-solid platforms.
    if (p.modeVis == 1 && cur != ShapeType::Cube)   return;
    if (p.modeVis == 2 && cur != ShapeType::Sphere) return;
    if (!p.solid) return;

    // Override colours for mode-specific platforms so they visually signal
    // which shape can use them.
    Color top  = p.top,  side = p.side;
    if (p.modeVis == 1) { top = MCubeCol(); side = ColorBrightness(MCubeCol(), -0.35f); }
    if (p.modeVis == 2) { top = MSphCol();  side = ColorBrightness(MSphCol(),  -0.35f); }

    DrawCube(p.pos, p.size.x, p.size.y, p.size.z, side);

    // Top cap raised slightly above the main body.
    float capH = 0.28f;
    Vector3 tp = { p.pos.x, p.pos.y + p.size.y*.5f + capH*.5f, p.pos.z };
    DrawCube(tp, p.size.x, capH, p.size.z, top);

    // Front face (+Z) thin overlay.
    float faceZ = p.pos.z + p.size.z*.5f;
    DrawCube({ p.pos.x, p.pos.y, faceZ+0.04f }, p.size.x, p.size.y, 0.08f,
             ColorBrightness(side, 0.1f));

    // Bottom trim (darkened).
    Vector3 bp = { p.pos.x, p.pos.y - p.size.y*.5f + 0.06f, p.pos.z };
    DrawCube(bp, p.size.x, 0.12f, p.size.z, ColorBrightness(side, -0.35f));

    DrawCubeWires(p.pos, p.size.x+.02f, p.size.y+.02f, p.size.z+.02f, {0,0,0,30});
}


//  DrawGhost — wireframe hint for mode-locked platforms

static void DrawGhost(const Platform& p, ShapeType cur)
{
    if (!p.solid) return;
    Color wc = { 0, 0, 0, 0 };
    if      (p.modeVis == 1 && cur != ShapeType::Cube)   wc = { 228, 88,  48, 90 };
    else if (p.modeVis == 2 && cur != ShapeType::Sphere) wc = { 48,  158, 238, 90 };
    else return;
    DrawCubeWires(p.pos, p.size.x, p.size.y, p.size.z, wc);
}

//  DrawSwitch — animated interactive switch prop

static void DrawSwitch(const Switch& sw, float time)
{
    float   bob = sinf(time * 3 + sw.pos.x) * .12f;   // Bob offset varies per switch.
    Vector3 p   = { sw.pos.x, sw.pos.y + bob, sw.pos.z };
    Color   c   = sw.on ? MSWon() : MSWoff();

    DrawCube(p, .75f, .75f, .75f, c);
    // Face plate — a brighter thin slab on the front.
    DrawCube({ p.x, p.y, p.z + .38f }, .46f, .46f, .05f, ColorBrightness(c, 0.4f));
    DrawCubeWires(p, .77f, .77f, .77f, BLACK);

    // Pulsing glow outline — draws attention without obstructing gameplay.
    float pulse = 0.5f + 0.5f * sinf(time * 4);
    DrawCubeWires(p,
        .94f + pulse*.13f, .94f + pulse*.13f, .94f + pulse*.13f,
        { c.r, c.g, c.b, (unsigned char)(45 + 30 * pulse) });
}


//  Background scenery helpers

/** DrawHill — a cluster of three overlapping spheres forming a rounded hillock. */
static void DrawHill(float cx, float cy, float cz, float r, Color c)
{
    DrawSphere({ cx,          cy,              cz }, r,          c);
    DrawSphere({ cx - r*.55f, cy - r*.25f,     cz }, r * .65f,  ColorBrightness(c, -0.05f));
    DrawSphere({ cx + r*.6f,  cy - r*.22f,     cz }, r * .60f,  ColorBrightness(c, -0.08f));
}


// DrawFlagpole — renders a checkpoint flag pole.

static void DrawFlagpole(const CP& cp)
{
    bool on = cp.on;
    DrawCylinder({ cp.pos.x, cp.pos.y-.6f, cp.pos.z }, 0.09f, 0.09f, 4.2f, 8, { 238,238,238,255 });
    DrawSphere  ({ cp.pos.x, cp.pos.y+3.6f, cp.pos.z }, 0.22f, on ? GOLD : GRAY);
    Color fc = on ? Color{255,72,48,255} : Color{140,140,140,200};
    DrawCube({ cp.pos.x+.38f, cp.pos.y+2.8f, cp.pos.z }, .60f, .55f, .06f, fc);
    if (on) DrawCube({ cp.pos.x+.38f, cp.pos.y+2.8f, cp.pos.z+.04f }, .22f, .22f, .06f, YELLOW);
    DrawCylinder({ cp.pos.x, cp.pos.y-.65f, cp.pos.z }, 0.42f, 0.35f, 0.18f, 8, { 165,165,165,230 });
}

/** DrawMtnBlock — a stylised mountain silhouette built from stacked cubes. */
static void DrawMtnBlock(float cx, float cy, float cz, float w, float h, Color c)
{
    DrawCube({ cx, cy+h*.28f, cz }, w,       h*.55f, w*.45f, ColorBrightness(c, -0.18f));
    DrawCube({ cx, cy+h*.62f, cz }, w*.65f,  h*.40f, w*.32f, c);
    DrawCube({ cx, cy+h*.88f, cz }, w*.36f,  h*.30f, w*.18f, ColorBrightness(c, 0.12f));
    DrawCube({ cx, cy+h,      cz }, w*.20f,  h*.18f, w*.10f, { 245,248,252,255 }); // Snow cap.
}

/** DrawCloud — a cluster of spheres arranged to read as a fluffy cloud. */
static void DrawCloud(float cx, float cy, float cz, float s, unsigned char a)
{
    Color cc = { 255,255,255,a }, cs = { 235,242,255,a };
    DrawSphere({ cx,            cy,            cz }, s,          cc);
    DrawSphere({ cx + s*.72f,  cy - s*.1f,    cz }, s*.70f,     cs);
    DrawSphere({ cx - s*.68f,  cy - s*.12f,   cz }, s*.66f,     cs);
    DrawSphere({ cx + s*.15f,  cy + s*.52f,   cz }, s*.50f,     cc);
    DrawSphere({ cx - s*.28f,  cy + s*.44f,   cz }, s*.42f,     cs);
}

// ---------------------------------------------------------------------------
//  DrawMenuScreen — title screen drawn entirely in 2-D (no BeginMode3D)
// ---------------------------------------------------------------------------
//
//  Layers from back to front:
//    1. Sky gradient.
//    2. Drifting 2-D cloud ellipses.
//    3. ORBICUBE title with pulsing colour.
//    4. Two shape-selection pills (purely decorative — both start the game).
//    5. Controls reference box.
//    6. Blinking "PRESS SPACE TO PLAY" prompt.

static void DrawMenuScreen(int sw, int sh, float time)
{
    DrawRectangleGradientV(0, 0, sw, sh, { 60,140,220,255 }, { 108,198,248,255 });

    // Drifting clouds: each cloud uses fmodf to wrap horizontally so they
    // loop continuously without any explicit reset logic.
    for (int i = 0; i < 5; i++) {
        float drift = fmodf(time * 18.f + i * 180.f, sw + 240.f) - 120.f;
        float cy2   = (float)(sh / 5 + i * 28);
        float sz    = 38.f + i * 8.f;
        unsigned char a = (unsigned char)(170 + i * 12);
        DrawEllipse((int)drift,             (int)cy2,      (int)sz,       (int)(sz*.45f), { 255,255,255,a });
        DrawEllipse((int)(drift - sz*.6f),  (int)(cy2+8),  (int)(sz*.7f), (int)(sz*.38f), { 255,255,255,(unsigned char)(a-30) });
        DrawEllipse((int)(drift + sz*.55f), (int)(cy2+6),  (int)(sz*.65f),(int)(sz*.36f), { 255,255,255,(unsigned char)(a-40) });
    }

    // Title — drop shadow then pulsing-colour foreground.
    const char* title = "ORBICUBE";
    int tfs = 86;
    int tw2 = MeasureText(title, tfs);
    int tx  = sw/2 - tw2/2, ty = sh/4;
    DrawText(title, tx+4, ty+5, tfs, { 0,0,0,80 });   // Shadow.
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

    // Mode pills (cosmetic).
    int pillY = ty + tfs + 70, pillW = 220, pillH = 52, gap = 24;
    int pillX = sw/2 - (pillW*2+gap)/2;
    DrawRectangleRounded({ (float)pillX,(float)pillY,(float)pillW,(float)pillH }, .4f, 8, MCubeCol());
    DrawRectangleRoundedLines({ (float)pillX,(float)pillY,(float)pillW,(float)pillH }, .4f, 8, { 0,0,0,60 });
    { int lw2 = MeasureText("2D  CUBE", 22); DrawText("2D  CUBE", pillX+pillW/2-lw2/2, pillY+15, 22, WHITE); }

    int sphX = pillX + pillW + gap;
    DrawRectangleRounded({ (float)sphX,(float)pillY,(float)pillW,(float)pillH }, .4f, 8, MSphCol());
    DrawRectangleRoundedLines({ (float)sphX,(float)pillY,(float)pillW,(float)pillH }, .4f, 8, { 0,0,0,60 });
    { int lw2 = MeasureText("3D  SPHERE", 22); DrawText("3D  SPHERE", sphX+pillW/2-lw2/2, pillY+15, 22, WHITE); }

    // Controls box.
    int boxW = 480, boxH = 160;
    int boxX = sw/2 - boxW/2, boxY = pillY + pillH + 36;
    DrawRectangleRounded({ (float)boxX,(float)boxY,(float)boxW,(float)boxH }, .22f, 8, { 0,0,0,145 });
    DrawRectangleRoundedLines({ (float)boxX,(float)boxY,(float)boxW,(float)boxH }, .22f, 8, { 255,255,255,40 });
    int lx2 = boxX+22, ly2 = boxY+16, ls = 20, lg = 28;
    DrawRectangle(lx2, ly2, 14, 14, MCubeCol()); DrawText("A / D - move   SPACE - jump",  lx2+22, ly2, ls, RAYWHITE); ly2 += lg;
    DrawRectangle(lx2, ly2, 14, 14, MSphCol());  DrawText("WASD - move ",                 lx2+22, ly2, ls, RAYWHITE); ly2 += lg;
    DrawRectangle(lx2, ly2, 14, 14, MSWon());    DrawText("[E] - activate switch",         lx2+22, ly2, ls, RAYWHITE); ly2 += lg;
    DrawText("[1] Cube   [2] Sphere   [R] Respawn   [TAB] Fullscreen", lx2, ly2, ls, { 200,200,200,200 });

    // Blinking prompt — alpha modulated by a slow sine wave.
    float blink = 0.55f + 0.45f * sinf(time * 3.8f);
    unsigned char ba = (unsigned char)(200 * blink);
    const char* ps  = "PRESS  SPACE  TO  PLAY";
    int psw2 = MeasureText(ps, 38);
    int psy  = boxY + boxH + 44;
    DrawText(ps, sw/2 - psw2/2 + 3, psy+3, 38, { 0,0,0,(unsigned char)(ba/3) });
    DrawText(ps, sw/2 - psw2/2, psy, 38, { 255,230,80,ba });

    DrawText("v5", sw-52, sh-28, 18, { 255,255,255,80 });
}

// ---------------------------------------------------------------------------
//  DrawWinScreen — victory overlay (drawn on top of the still-running 3-D world)
// ---------------------------------------------------------------------------

static void DrawWinScreen(int sw, int sh, float time)
{
    // Semi-transparent dark overlay dims the 3-D scene behind this UI.
    DrawRectangle(0, 0, sw, sh, { 0, 0, 0, 175 });

    // Confetti: 40 randomised coloured rectangles flying upward using a
    // deterministic seed so they're visually consistent without storing state.
    for (int i = 0; i < 40; i++) {
        float seed = (float)i * 137.508f;   // Golden angle step — good distribution.
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
    // Horizontal shake driven by sin for a celebratory wobble.
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

// =============================================================================
// main() — window init, world setup, game loop
// =============================================================================

int main()
{
    // ── Window & display ──────────────────────────────────────────────────────
    SetConfigFlags(FLAG_MSAA_4X_HINT);   // Request 4× MSAA for smoother edges.
    InitWindow(1920, 1080, "ORBICUBE");
    SetTargetFPS(180);
    DisableCursor();                     // Hide the OS cursor; camera is mouse-driven.
    InitAudioDevice();
    ToggleFullscreen();

    // ── Audio synthesis ───────────────────────────────────────────────────────
    //  All sounds are generated here, loaded into Sound objects, and the raw
    //  Wave data freed immediately — only the GPU-side Sound persists.
    Wave wJ  = MakeSweep(300, 600,  .10f, .42f, .003f); Sound sJ   = LoadSoundFromWave(wJ);  UnloadWave(wJ);   // Jump
    Wave wL  = MakeSweep(160,  52,  .11f, .50f, .002f); Sound sL   = LoadSoundFromWave(wL);  UnloadWave(wL);   // Land
    Wave wM  = MakeSweep(440, 880,  .16f, .35f, .008f); Sound sM   = LoadSoundFromWave(wM);  UnloadWave(wM);   // Morph / shape switch
    Wave wSw = MakeTone (940,        .08f, .38f, 1, .002f, .05f); Sound sSw = LoadSoundFromWave(wSw); UnloadWave(wSw); // Switch toggle
    Wave wS1 = MakeSweep(523, 784,  .28f, .38f, .008f); Sound sS1  = LoadSoundFromWave(wS1); UnloadWave(wS1);  // (unused — reserved)
    Wave wS2 = MakeSweep(659, 988,  .28f, .28f, .016f); Sound sS2  = LoadSoundFromWave(wS2); UnloadWave(wS2);  // (unused — reserved)
    Wave wCP = MakeTone (660,        .22f, .32f, 0, .008f, .18f); Sound sCP = LoadSoundFromWave(wCP); UnloadWave(wCP); // Checkpoint
    Wave wSt = MakeTone (220,        .04f, .10f, 3, .001f, .02f); Sound sSt = LoadSoundFromWave(wSt); UnloadWave(wSt); // Footstep (noise burst)
    Wave wW1 = MakeSweep(440,  1320, .55f, .45f, .012f); Sound sWin1 = LoadSoundFromWave(wW1); UnloadWave(wW1); // Win fanfare low
    Wave wW2 = MakeSweep(660,  1760, .55f, .32f, .025f); Sound sWin2 = LoadSoundFromWave(wW2); UnloadWave(wW2); // Win fanfare high

    float stepT = 0;   // Countdown between footstep sound triggers.

    // ── World geometry ────────────────────────────────────────────────────────
    //
    //  World units: 1 unit ≈ 1 tile-width.
    //  Platforms grow rightward along +X.  The player starts at X=0 and the
    //  final checkpoint is at X≈155.
    //
    //  Three helper lambdas reduce boilerplate for the common platform types:
    //    P  — generic platform with explicit colours and Z offset.
    //    PC — cube-only platform (orange tint, modeVis=1, Z=0).
    //    PS — sphere-only platform (blue tint, modeVis=2, explicit Z).

    const float T  = 2.f;    // Standard platform thickness (Y size).
    const float G  = 0;      // Ground level (Y=0 after the PY offset is applied).
    const float H1 = 3.f;    // Low raised height.
    const float H2 = 6.f;    // High raised height.

    // PY converts a surface height to a platform centre Y by subtracting 1
    // so the player lands flush with the top face (which is at pos.y + size.y/2).
    auto PY = [](float s) { return s - 1.f; };

    std::vector<Platform> world;

    auto P = [&](float x, float y, float w, float d, float z, Color top, Color side) {
        Platform pl = { {x, PY(y), z}, {w, T, d}, top, side, 0, true };
        pl.origZ = z;
        world.push_back(pl);
    };
    auto PC = [&](float x, float y, float w, float d) {
        Platform pl = { {x, PY(y), 0}, {w, T, d}, MCubeCol(), ColorBrightness(MCubeCol(), -.3f), 1, true };
        pl.origZ = 0;
        world.push_back(pl);
    };
    auto PS = [&](float x, float y, float w, float d, float z) {
        Platform pl = { {x, PY(y), z}, {w, T, d}, MSphCol(), ColorBrightness(MSphCol(), -.3f), 2, true };
        pl.origZ = z;
        world.push_back(pl);
    };

    // ── Zone 0: Grassland (tutorial area) ────────────────────────────────────
    //  Introduces basic movement.  Gentle terrain, a cube-only ledge and a
    //  sphere-only raised platform teach the player about mode-specific paths.
    P(  0, G,  16, 16,  0, MGrass(), MDirt());   // Starting island.
    P( -8, G,   6, 16,  0, MGrass(), MDirt());   // Left extension.
    P( 30, G,  10, 12,  0, MGrass(), MDirt());   // First gap island.
    P( 44, H2,  8, 12,  0, MGrass(), MDirt());   // Elevated platform.
    P( 56, G,  10, 14,  0, MGrass(), MDirt());   // Switch platform.
    PC(13, G,   4,  6);                          // Cube-only ledge (mode-gated shortcut).
    PS(34, H1,  8,  6,  8);                      // Sphere-only raised platform (off-axis in Z).

    // ── Zone 1: Desert (bridge puzzle) ───────────────────────────────────────
    //  The bridge starts non-solid.  Activating the switch in Zone 0 makes
    //  the bridge solid AND starts the MovingPlat animation.
    int bridgeIdx = (int)world.size();   // Record index before pushing.
    Platform bridge = { {66, PY(G), 0}, {9, T, 10}, MSand(), MDkSand(), 0, false };
    bridge.origZ = 0;
    world.push_back(bridge);
    P(89, G,  8, 11, 25, MGrass(), MDirt());     // Island after the bridge (offset in Z).

    // ── Zone 2: Forest (moving platforms + 3-D puzzle) ────────────────────────
    P(105, G,  10, 13,   0, MGrass(), MDirt());
    P(119, H1,  8, 10,   0, MWood(),  MDkWood());
    P(143, H1,  8, 10,   0, MWood(),  MDkWood());
    P(155, G,  12, 14,   0, MGrass(), MDirt());    // Final checkpoint platform.
    PS(131, H2, 8,  6, -11);                       // Sphere-only shortcut (negative Z).

    // ── Switches ──────────────────────────────────────────────────────────────
    //  Switch is placed on top of the Zone 0 switch platform (X=56).
    //  The Y offset places it flush on the platform surface.
    std::vector<Switch> switches;
    switches.push_back({ {57, PY(G) + T/2 + 0.85f, 0}, false, 10, {230,210,50,255} });

    // ── Moving platforms ──────────────────────────────────────────────────────
    //  One mover controls the bridge: it travels from X=67 to X=80 at 7 u/s.
    //  `active=false` initially; toggled true by the switch.
    std::vector<MovingPlat> movPlats;
    movPlats.push_back({ bridgeIdx, 67.f, 80.f, 7.f, 62.f, 62.f, 1, false });

    // ── Checkpoints ───────────────────────────────────────────────────────────
    //  Four checkpoints spaced across the three zones.  The first is
    //  pre-activated (player starts here).
    std::vector<CP> cps;
    cps.push_back({ {  0, 1, 0}, true  });    // Zone 0 start.
    cps.push_back({ { 55, 1, 0}, false });    // Zone 0/1 boundary.
    cps.push_back({ {105, 1, 0}, false });    // Zone 1/2 boundary.
    cps.push_back({ {155, 1, 0}, false });    // Zone 2 finish.
    int activeCp  = 0;
    const int LAST_CP = (int)cps.size() - 1;

    // ── Background scenery ────────────────────────────────────────────────────
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
    std::vector<MtnObj> mtns = {
        {-10,0,-55,25,20},{22,0,-58,22,17},{58,0,-60,28,23},
        {98,0,-56,24,20},{140,0,-58,28,22},{175,0,-55,22,18},
    };

    // ── Camera setup ──────────────────────────────────────────────────────────
    //  The camera switches projection mode based on player shape:
    //    Cube   → orthographic (true 2-D side view; no perspective distortion).
    //    Sphere → perspective with mouse-driven yaw/pitch orbit.
    Camera3D cam = {};
    cam.fovy = 60; cam.up = { 0,1,0 }; cam.projection = CAMERA_PERSPECTIVE;

    Player player; player.pos = { 0, 3, 0 };

    float yaw   = 0;       // Horizontal camera angle (radians), mouse X.
    float pitch = 0.30f;   // Vertical camera angle (radians), mouse Y.
    float time  = 0;       // Accumulated elapsed time (seconds), used for animations.

    std::string hint;      // One-line contextual HUD message (checkpoint, switch, etc.).
    float hintT = 0;       // Countdown (seconds) until `hint` fades out.

    float shakeT = 0;      // Camera-shake countdown.

    // Zone tracking — used to cross-fade the sky colour and display the room name.
    int   curRoom  = 0, lastRoom = -1;
    float roomFadeT = 0;   // Fade-in timer for the room-name label.

    std::vector<Flash> flashes;   // Active full-screen colour flashes.

    float modePillT = 0;   // 0=cube side lit, 1=sphere side lit; lerped for the HUD pill.

    // Smooth camera state (lerped each frame to avoid jitter).
    Vector3 camPos = { 0, 7, 24 }, camTgt = { 0, 2, 0 };
    float   camFov = 60;

    bool wasOnGround = false;   // Landing-detection: stores last frame's ground state.

    // Per-zone sky colours — linearly interpolated as the player moves between zones.
    Color skyTgts[3] = {
        { 108, 198, 248, 255 },   // Grassland — bright blue sky.
        { 218, 188, 128, 255 },   // Desert    — warm sandy haze.
        {  88, 158,  88, 255 },   // Forest    — muted green canopy light.
    };
    Color curSky = skyTgts[0];

    const char* roomNames[] = { "Grassland", "Desert Ruins", "Forest" };

    GameState state   = GameState::Menu;
    float     winTime = 0.f;

    // ── ResetGame lambda ──────────────────────────────────────────────────────
    //  Restores all mutable game state to its initial configuration.
    //  Called at startup (Menu→Playing) and when pressing R on the Win screen.
    auto ResetGame = [&]()
    {
        player       = Player();
        player.pos   = { 0, 3, 0 };
        activeCp     = 0;

        // Reset all checkpoints: only the first is active.
        for (auto& cp : cps) cp.on = false;
        cps[0].on = true;

        // Reset all switches to off, and disable the bridge.
        for (auto& sw : switches) sw.on = false;
        for (auto& mp : movPlats) { mp.active = false; mp.cur = mp.minX; mp.dir = 1; }
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

    // =========================================================================
    //  Game loop
    // =========================================================================

    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();
        if (dt > 0.05f) dt = 0.05f;   // Cap delta-time to prevent spiral-of-death at low FPS.
        time += dt;

        int scrW = GetScreenWidth(), scrH = GetScreenHeight();

        // ── Menu state ────────────────────────────────────────────────────────
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
            continue;   // Skip the rest of the loop; no physics needed on the menu.
        }

        // ── Win state ─────────────────────────────────────────────────────────
        if (state == GameState::Win)
        {
            winTime += dt;
            if (IsKeyPressed(KEY_R)) { ResetGame(); state = GameState::Playing; }

            // Still render the world in the background so the win screen
            // overlays a live scene rather than a blank canvas.
            BeginDrawing();
            ClearBackground({ 108, 198, 248, 255 });
            BeginMode3D(cam);
            rlEnableDepthTest(); rlEnableDepthMask();
            rlDisableBackfaceCulling();
            DrawSphere({ 135,0,0 }, 700, { curSky.r,curSky.g,(unsigned char)(curSky.b+10),255 });
            rlEnableBackfaceCulling();
            DrawPlane({ 135,-1.32f,0 }, { 580,140 }, { 82,195,65,255 });
            for (auto& hl : hills)  DrawHill(hl.x, hl.y, hl.z, hl.r, { 75,195,62,255 });
            for (auto& m  : mtns)   DrawMtnBlock(m.cx, m.cy, m.cz, m.w, m.h, { 138,142,148,255 });
            for (auto& cl : clouds) { float drift = sinf(time*.10f + cl.phase)*3.f; DrawCloud(cl.cx+drift, cl.cy, cl.cz, cl.s, 212); }
            for (auto& p  : world)  DrawPlatform(p, player.shape);
            for (auto& sw : switches) DrawSwitch(sw, time);
            for (auto& cp : cps)    DrawFlagpole(cp);
            DrawParts();
            player.Draw();
            EndMode3D();
            DrawWinScreen(scrW, scrH, winTime);
            EndDrawing();
            continue;
        }

        // =====================================================================
        //  Playing state — per-frame logic
        // =====================================================================

        // Decay timers.
        shakeT = Clamp(shakeT - dt, 0, 10);
        hintT  = Clamp(hintT  - dt, 0, 10);

        // ── Camera mouse look (Sphere mode) ───────────────────────────────────
        //  Mouse delta drives yaw (left/right) and pitch (up/down).
        //  Pitch is clamped to avoid flipping over the top or bottom.
        Vector2 md = GetMouseDelta();
        yaw   += md.x * .003f;
        pitch  = Clamp(pitch - md.y * .003f, -0.95f, 0.95f);

        // ── Input → movement direction ────────────────────────────────────────
        //  Cube mode uses a simple 1-D signed axis (rawX).
        //  Sphere mode uses camera-relative 2-D input transformed by yaw so
        //  "forward" always means "the direction the camera is looking".
        float   rawX = (float)(IsKeyDown(KEY_D) - IsKeyDown(KEY_A));
        Vector2 inp  = {
            (float)(IsKeyDown(KEY_A) - IsKeyDown(KEY_D)),
            (float)(IsKeyDown(KEY_S) - IsKeyDown(KEY_W))
        };
        if (Vector2Length(inp) > 1) inp = Vector2Normalize(inp);  // Diagonal normalisation.

        // Rotate the 2-D input by yaw to get world-space XZ direction.
        Vector3 sphDir = {
             inp.x * cosf(yaw) - inp.y * sinf(yaw), 0,
            -inp.x * sinf(yaw) - inp.y * cosf(yaw)
        };
        if (Vector3Length(sphDir) > 0.01f) sphDir = Vector3Normalize(sphDir);

        // ── Player input & physics ─────────────────────────────────────────────
        bool jumped = false, switched = false;
        wasOnGround = player.onGround;
        player.HandleInput(dt, rawX, sphDir, jumped, switched);
        if (jumped)   PlaySound(sJ);
        if (switched) PlaySound(sM);

        // In Cube mode, hard-lock Z position and velocity every frame to
        // prevent floating-point drift from pushing the player off-axis.
        if (player.shape == ShapeType::Cube) { player.pos.z = 0; player.vel.z = 0; }

        player.Update(dt);
        UpdateParts(dt);

        // ── Platform Z animation ───────────────────────────────────────────────
        //  In Cube mode all platforms slide to Z=0 (flattened into the 2-D plane).
        //  In Sphere mode they lerp back to their `origZ` design positions.
        for (auto& pl : world)
            pl.pos.z = Lerp(pl.pos.z, (player.shape == ShapeType::Cube) ? 0.f : pl.origZ, 9.f * dt);

        // ── Moving platform animation ──────────────────────────────────────────
        for (auto& mp : movPlats)
        {
            mp.prevX = mp.cur;
            if (!mp.active) continue;

            mp.cur += mp.dir * mp.speed * dt;
            // Bounce at endpoints.
            if (mp.cur >= mp.maxX) { mp.cur = mp.maxX; mp.dir = -1; }
            if (mp.cur <= mp.minX) { mp.cur = mp.minX; mp.dir =  1; }

            if (mp.idx >= 0 && mp.idx < (int)world.size())
                world[mp.idx].pos.x = mp.cur;   // Apply to the actual Platform.
        }

        // ── Collision detection & resolution ───────────────────────────────────
        //  Test each solid/mode-matching platform against the player.
        //  Resolve() pushes the player out of any penetration; OnLand() is
        //  triggered for floor contacts (normal.y > 0.7 AND downward velocity).
        float preVelY = player.vel.y;
        for (auto& pl : world)
        {
            if (!PlatActive(pl, player.shape)) continue;
            Hit h = BoxHit(player, pl);
            if (!h.yes) continue;
            bool isFloor = (h.n.y > 0.7f && preVelY <= 0);
            Resolve(player, h);
            if (isFloor) player.OnLand(pl.pos.y + pl.size.y*.5f, fabsf(preVelY));
        }

        // ── Moving platform carry ──────────────────────────────────────────────
        //  If the player is standing on a moving platform, add that platform's
        //  X delta to the player's X this frame so they move together.
        //  The platform-top proximity test prevents carrying at a distance.
        for (auto& mp : movPlats)
        {
            if (!mp.active || !player.onGround) continue;
            float delta = mp.cur - mp.prevX;
            if (fabsf(delta) < 0.0001f) continue;

            auto& pl    = world[mp.idx];
            float halfW = pl.size.x*.5f +
                (player.shape == ShapeType::Sphere ? player.radius : player.size.x*.5f);
            float dx = fabsf(player.pos.x - pl.pos.x);
            float dy = player.pos.y - (pl.pos.y + pl.size.y*.5f);

            // Only carry if close enough laterally and vertically.
            if (dx < halfW && dy > -0.15f && dy < 1.8f)
                player.pos.x += delta;
        }

        // ── Landing sound & particles ──────────────────────────────────────────
        //  Detect the frame of landing (was airborne, now grounded) and trigger
        //  audio + visual feedback scaled to the impact speed.
        if (!wasOnGround && player.onGround)
        {
            float impact = fabsf(preVelY);
            if (impact > 1.5f) {
                // Pitch the land sound up slightly for harder impacts.
                SetSoundPitch(sL, 0.82f + Clamp(impact / 22.f, 0.f, 0.55f));
                PlaySound(sL);
                player.SpawnLandParticles(gParts, impact);
            }
        }

        // ── Footstep sounds ───────────────────────────────────────────────────
        //  A noise burst plays on a timer while the cube is moving on the ground.
        //  The interval and pitch scale with speed for a natural feel.
        if (player.shape == ShapeType::Cube && player.onGround && fabsf(player.vel.x) > 1.2f)
        {
            stepT -= dt;
            if (stepT <= 0) {
                float sr = fabsf(player.vel.x) / 9.f;
                SetSoundPitch(sSt, 0.78f + sr * 0.45f);
                PlaySound(sSt);
                stepT = 0.26f - sr * 0.07f;   // Shorter interval at higher speeds.
            }
        }
        else stepT = 0;

        // ── Respawn ───────────────────────────────────────────────────────────
        //  Triggered if the player falls below the checkpoint height by 12 units
        //  OR manually by pressing [R].
        if (player.pos.y < cps[activeCp].pos.y - 12 || IsKeyPressed(KEY_R))
        {
            player.pos = cps[activeCp].pos;
            player.vel = { 0, 0, 0 };
            player.onGround = false;
            flashes.push_back({ RED, 0.28f, 0.28f });   // Brief red flash.
        }

        // ── Checkpoint activation ─────────────────────────────────────────────
        for (int i = 0; i < (int)cps.size(); i++)
        {
            if (cps[i].on) continue;
            // Simple radius check (13 units squared → ~3.6 unit radius).
            if (Vector3LengthSqr(Vector3Subtract(player.pos, cps[i].pos)) < 13)
            {
                cps[i].on = true;
                if (i > activeCp) activeCp = i;   // Always advance to the further CP.

                if (i == LAST_CP)
                {
                    // ── Win condition ──────────────────────────────────────
                    flashes.push_back({ GOLD, 1.2f, 1.2f });
                    player.SpawnCollectParticles(gParts, GOLD);
                    PlaySound(sWin1); PlaySound(sWin2);
                    state   = GameState::Win;
                    winTime = 0.f;
                }
                else
                {
                    // ── Intermediate checkpoint ────────────────────────────
                    hint = "Checkpoint!"; hintT = 2.5f;
                    flashes.push_back({ {72,255,138,255}, 0.30f, 0.30f });
                    PlaySound(sCP);
                }
            }
        }

        // ── Switch interaction [E] ─────────────────────────────────────────────
        if (IsKeyPressed(KEY_E))
        {
            for (auto& sw : switches)
            {
                // Proximity check: 11 units squared → ~3.3 unit range.
                if (Vector3LengthSqr(Vector3Subtract(player.pos, sw.pos)) > 11.f) continue;

                sw.on = !sw.on;

                // Propagate the switch state to any mover that controls the bridge.
                for (auto& mp : movPlats)
                    if (mp.idx == bridgeIdx) { mp.active = sw.on; world[mp.idx].solid = sw.on; }

                hint  = sw.on ? "Switch ON" : "Switch OFF";
                hintT = 3; shakeT = 0.2f;
                Color fc = sw.on ? MSWon() : MSWoff();
                flashes.push_back({ fc, 0.32f, 0.32f });
                PlaySound(sSw);
                player.SpawnCollectParticles(gParts, fc);
            }
        }

        // ── HUD mode pill ─────────────────────────────────────────────────────
        //  modePillT tracks which half of the pill is "lit"; lerped for a
        //  smooth slide animation rather than a hard snap.
        modePillT = Lerp(modePillT, (player.shape == ShapeType::Sphere) ? 1.f : 0.f, 12*dt);

        // ── Zone tracking & sky colour interpolation ───────────────────────────
        float px2 = player.pos.x;
        if      (px2 < 52)  curRoom = 0;
        else if (px2 < 105) curRoom = 1;
        else                curRoom = 2;

        if (curRoom != lastRoom) { roomFadeT = 1.4f; lastRoom = curRoom; }
        if (roomFadeT > 0) roomFadeT -= dt;

        // Lerp each sky colour channel independently toward the target zone colour.
        Color ts = skyTgts[curRoom];
        curSky.r = (unsigned char)Lerp((float)curSky.r, (float)ts.r, 1.8f*dt);
        curSky.g = (unsigned char)Lerp((float)curSky.g, (float)ts.g, 1.8f*dt);
        curSky.b = (unsigned char)Lerp((float)curSky.b, (float)ts.b, 1.8f*dt);

        // ── Camera update ──────────────────────────────────────────────────────
        if (player.shape == ShapeType::Cube)
        {
            // Orthographic 2-D: camera follows the player in X with a small
            // look-ahead (lax) so the player has more "view ahead" of their motion.
            float lax = player.vel.x * 0.26f;
            camPos = Vector3Lerp(camPos, { player.pos.x + lax, player.pos.y + 5.5f, 27.f }, 8.f*dt);
            camTgt = Vector3Lerp(camTgt, { player.pos.x + lax*.3f, player.pos.y + 1.2f, 0.f }, 8.f*dt);
            cam.position   = camPos; cam.target = camTgt; cam.up = { 0, 1, 0 };
            cam.projection = CAMERA_ORTHOGRAPHIC; cam.fovy = 14.f;
        }
        else
        {
            // Perspective 3-D: camera orbits the player using spherical coordinates
            // derived from yaw and pitch.  A small shake offset is added on heavy
            // landings or switch activations.
            Vector3 shake = {};
            if (shakeT > 0)
                shake = { (float)GetRandomValue(-2,2)*.016f, (float)GetRandomValue(-1,1)*.010f, 0 };

            Vector3 la  = { player.vel.x*.17f, player.vel.y*.05f, player.vel.z*.17f };
            Vector3 tgt = Vector3Add(Vector3Add(player.pos, la), shake);

            // Camera forward vector from spherical (yaw, pitch).
            Vector3 cf = { cosf(pitch)*sinf(yaw), sinf(pitch), cosf(pitch)*cosf(yaw) };

            // Smooth follow: target and position lerp independently for a
            // slightly delayed, cinematic feel.
            camTgt = Vector3Lerp(camTgt, tgt, 0.12f);
            camPos = Vector3Lerp(camPos, Vector3Add(tgt, Vector3Scale(cf, -14.f)), 0.10f);

            // Dynamic FOV: widens when moving fast for a speed-rush sensation.
            float hs = sqrtf(player.vel.x*player.vel.x + player.vel.z*player.vel.z);
            camFov = Lerp(camFov, 60.f + hs * 0.48f, 4.f * dt);

            cam.position   = camPos; cam.target = camTgt; cam.up = { 0,1,0 };
            cam.projection = CAMERA_PERSPECTIVE; cam.fovy = camFov;
        }

        // Tick and prune flashes.
        for (auto& f : flashes) f.t -= dt;
        flashes.erase(
            std::remove_if(flashes.begin(), flashes.end(), [](const Flash& f){ return f.t <= 0; }),
            flashes.end());

        // =====================================================================
        //  Render
        // =====================================================================

        BeginDrawing();
        ClearBackground(curSky);
        BeginMode3D(cam);
        rlEnableDepthTest(); rlEnableDepthMask();

        // ── 3-D background scene ───────────────────────────────────────────────
        //  In Sphere mode the full 3-D skybox and scenery is rendered.
        //  In Cube mode a simple flat ground plane suffices — the scene is
        //  essentially 2-D so the parallax depth of 3-D scenery would be distracting.
        if (player.shape == ShapeType::Sphere)
        {
            // Sky sphere: a large inverted sphere with back-face culling disabled
            // so the inside face is visible.
            rlDisableBackfaceCulling();
            DrawSphere({ 135,0,0 }, 700,
                { curSky.r, curSky.g, (unsigned char)(curSky.b+10), 255 });
            rlEnableBackfaceCulling();

            // Horizon haze band and ground plane.
            DrawCube({ 135,-2,0 }, 620, 2.5f, 110,
                { curSky.r, (unsigned char)(curSky.g+5), (unsigned char)(curSky.b+16), 255 });
            DrawPlane({ 135,-1.32f, 0 }, { 580, 140 }, { 82,195,65,255 });
            DrawPlane({ 135,-1.28f,-45 }, { 580, 70 }, { 65,178,50,255 });

            // Biome-tinted hills, mountains, and clouds.
            Color hillCol = { 75,195,62,255 };
            if (curRoom == 1) hillCol = { 165,148,95,255 };
            if (curRoom == 2) hillCol = { 55,138,45,255 };
            for (auto& hl : hills) DrawHill(hl.x, hl.y, hl.z, hl.r, hillCol);

            Color mc = { 138,142,148,255 };
            if (curRoom == 1) mc = { 168,152,108,255 };
            if (curRoom == 2) mc = { 85,128,72,255 };
            for (auto& m : mtns) DrawMtnBlock(m.cx, m.cy, m.cz, m.w, m.h, mc);

            // Clouds drift slowly using a sine offset based on each cloud's phase.
            for (auto& cl : clouds) {
                float drift = sinf(time*.10f + cl.phase) * 3.f;
                DrawCloud(cl.cx + drift, cl.cy, cl.cz, cl.s, 212);
            }
        }
        else
        {
            // Cube mode: just a flat green ground plane.
            DrawPlane({ 135,-1.2f,0 }, { 480,120 }, { 90,188,70,255 });
        }

        // ── World geometry ─────────────────────────────────────────────────────
        for (auto& p : world) DrawPlatform(p, player.shape);
        for (auto& p : world) DrawGhost(p, player.shape);   // Ghost wireframes for inaccessible platforms.
        rlEnableDepthMask();
        for (auto& sw : switches) DrawSwitch(sw, time);
        for (auto& cp : cps)      DrawFlagpole(cp);
        DrawParts();
        player.Draw();
        EndMode3D();

        // ── Full-screen flashes ────────────────────────────────────────────────
        //  Each flash decays in opacity as its remaining time drops below half
        //  its duration; the effect is brief but noticeable.
        for (auto& f : flashes) {
            float prog = f.t / f.dur;
            unsigned char fa = (unsigned char)(Clamp(prog * 2.f, 0.f, 1.f) * 65);
            DrawRectangle(0, 0, scrW, scrH, { f.col.r, f.col.g, f.col.b, fa });
        }

        // ── Screen-edge vignette ───────────────────────────────────────────────
        //  Multiple semi-transparent black border rectangles with increasing
        //  width create a smooth darkening toward the screen edges.
        for (int i = 0; i < 7; i++) {
            float  t2 = (float)i / 7;
            unsigned char a = (unsigned char)(t2 * t2 * 95);
            DrawRectangleLinesEx(
                { -(float)i*3, -(float)i*3, (float)scrW+i*6, (float)scrH+i*6 },
                (float)(i+1)*6, { 0,0,0,a });
        }

        // ── Room name transition fade ──────────────────────────────────────────
        if (roomFadeT > 0) {
            float p2  = roomFadeT / 1.4f;
            // Only darken during the first 28% of the fade (the "slam to black" phase).
            float fa2 = p2 > 0.72f ? (p2 - 0.72f) / 0.28f : 0;
            DrawRectangle(0, 0, scrW, scrH, { 0,0,0,(unsigned char)(fa2 * 140) });
        }

        // ── HUD: Zone name label ───────────────────────────────────────────────
        DrawRectangleRounded({ (float)scrW/2 - 148, 10, 296, 47 }, .3f, 6, { 0,0,0,148 });
        {
            int tw2 = MeasureText(roomNames[curRoom], 24);
            DrawText(roomNames[curRoom], scrW/2 - tw2/2, 20, 24, RAYWHITE);
        }

        // ── HUD: Mode pill (2D / 3D toggle indicator) ─────────────────────────
        //  A sliding highlight shows which mode is active; modePillT (0–1)
        //  drives the X offset of the highlight rectangle.
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

        // ── HUD: Mode advantage label ─────────────────────────────────────────
        //  Reminds the player what each shape excels at.
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

        // ── HUD: Switch proximity prompt ──────────────────────────────────────
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

        // ── HUD: Controls reference panel ─────────────────────────────────────
        DrawRectangleRounded({ 10, (float)scrH-100, 378, 88 }, .25f, 6, { 0,0,0,148 });
        DrawRectangle(18, scrH-90, 14, 14, MCubeCol()); DrawText("Cube 2D: A/D move",                         36, scrH-92, 17, RAYWHITE);
        DrawRectangle(18, scrH-70, 14, 14, MSphCol());  DrawText("Sphere 3D: WASD",                            36, scrH-72, 17, RAYWHITE);
        DrawRectangle(18, scrH-50, 14, 14, MSWon());    DrawText("Switch [E]  R=respawn  TAB=fullscreen",      36, scrH-52, 17, RAYWHITE);
        DrawText("SPACE = jump (hold for higher)", scrW/2 - 165, scrH-24, 18, { 255,255,255,128 });

        // ── HUD: Context hint banner (checkpoint, switch, etc.) ───────────────
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
    }   // end game loop

    // ── Cleanup ────────────────────────────────────────────────────────────────
    UnloadSound(sJ);  UnloadSound(sL);   UnloadSound(sM);
    UnloadSound(sSw); UnloadSound(sS1);  UnloadSound(sS2);
    UnloadSound(sCP); UnloadSound(sSt);  UnloadSound(sWin1); UnloadSound(sWin2);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}