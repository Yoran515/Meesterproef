// =============================================================================
//  Player.cpp  —  Player physics, input, and rendering implementation
// =============================================================================
//
//  Sections
//  ────────
//  1. Physics constants
//  2. Constructor
//  3. HandleInput   — keyboard → velocity
//  4. Update        — integrate physics, age trail
//  5. OnLand        — collision landing response
//  6. SpawnLandParticles / SpawnCollectParticles
//  7. AddTrailSample (private)
//  8. Draw          — 3-D rendering of cube, sphere, trail
//
// =============================================================================

#include "Player.h"
#include "raymath.h"
#include "rlgl.h"
#include <cmath>
#include <vector>

// =============================================================================
//  1. Physics constants
// =============================================================================
//
//  Two movement "personalities" exist — one for Cube (tight 2-D platformer
//  feel) and one for Sphere (floaty 3-D roll).
//
//  Cube
//  ────
//  GRAVITY     — downward acceleration in units/s².  Shared by both shapes.
//  JUMP_FORCE  — initial upward velocity on jump (before hold bonus).
//  CUBE_MAX    — maximum horizontal speed (units/s).
//  CUBE_ACC    — acceleration rate when a direction key is held.
//                High value → near-instant speed; gives a snappy, precise feel.
//  CUBE_DEC    — deceleration rate when no key is held.
//                Slightly higher than ACC so the player stops a touch faster
//                than it accelerates, avoiding "sliding on ice" feel.
//
//  Sphere
//  ──────
//  SPH_MAX     — maximum horizontal speed (slightly faster than cube).
//  SPH_ACC     — acceleration per second toward the target velocity.
//                Very low relative to CUBE_ACC; builds speed gradually for a
//                weighty, momentum-heavy feel.
//  SPH_DEC     — passive friction (lerp toward zero) when no input given.
//                Low value preserves momentum intentionally.

static constexpr float GRAVITY    = 26.f;
static constexpr float JUMP_FORCE = 10.5f;

static constexpr float CUBE_MAX = 9.0f,  CUBE_ACC = 58.f, CUBE_DEC = 70.f;
static constexpr float SPH_MAX  = 11.5f, SPH_ACC  = 9.8f, SPH_DEC  = 2.5f;

// =============================================================================
//  2. Constructor — safe default state
// =============================================================================

Player::Player()
{
    // Place the player just above the ground so the first physics frame
    // doesn't intersect the floor before collision runs.
    pos = { 0, 3, 0 };
    vel = { 0, 0, 0 };

    // Cube dimensions match a typical humanoid-ish 1×2×1 block.
    size = { 1, 2, 1 };

    onGround = false;

    // Sphere radius chosen so the sphere's silhouette visually fits within the
    // 1×2 cube footprint when morphing between shapes.
    radius    = 0.85f;
    rollAngle = 0;
    rollAxis  = { 1, 0, 0 };   // Arbitrary starting axis; updated on first move.

    shape     = ShapeType::Cube;

    // morphT = 0  → fully cube appearance.
    // morphSpeed controls lerp rate; 8 gives a ~0.1-second transition at 60 fps.
    morphT     = 0;
    morphSpeed = 8;

    // All jump-state timers start at zero (no buffered jump, no coyote grace).
    coyoteTimer  = 0;
    jumpBuffer   = 0;
    jumpHoldTimer = 0;
    jumpHeld     = false;

    // Trail ring-buffer starts empty.
    trailHead  = 0;
    trailCount = 0;
    trailTimer = 0;

    landFlash = 0;
}

// =============================================================================
//  3. HandleInput — translate keyboard state into velocity changes
// =============================================================================
//
//  Design notes
//  ────────────
//  • Input is polled every frame (not event-driven) so held keys affect
//    velocity continuously via Lerp/Clamp rather than impulses.
//  • Jump is split across two systems:
//      a) jumpBuffer  — queues a jump on key press even if not yet grounded.
//      b) jumpHeld    — applies bonus upward force while Space remains held,
//                       allowing variable jump height (tap = short, hold = tall).
//  • The `outJumped` / `outSwitched` booleans let main.cpp trigger audio
//    without the Player needing to know about Sound objects.

void Player::HandleInput(float dt, float rawDirX, Vector3 sphereDir,
                         bool& jmp, bool& sw)
{
    jmp = sw = false;

    // ── Shape switching ──────────────────────────────────────────────────────
    // [1] → Cube: lock Z velocity to prevent drifting after the switch.
    if (IsKeyPressed(KEY_ONE) && shape != ShapeType::Cube)
    {
        shape  = ShapeType::Cube;
        size   = { 1, 2, 1 };
        vel.z  = 0;
        sw     = true;
    }
    // [2] → Sphere: expand the bounding box to match the sphere radius so
    //   collision detection (which uses AABB for both shapes) stays accurate.
    if (IsKeyPressed(KEY_TWO) && shape != ShapeType::Sphere)
    {
        shape  = ShapeType::Sphere;
        radius = 0.85f;
        size   = { 1.7f, 1.7f, 1.7f };
        sw     = true;
    }

    // [TAB] — toggle fullscreen (delegated to Raylib).
    if (IsKeyPressed(KEY_TAB)) ToggleFullscreen();

    // ── Jump buffering ───────────────────────────────────────────────────────
    // On Space press, record the intent; the actual jump fires below only when
    // the player is grounded (or within coyote time).
    if (IsKeyPressed(KEY_SPACE)) jumpBuffer = JUMP_BUFFER_TIME;

    // Fire the jump if:
    //   • There is a buffered intent (jumpBuffer > 0), AND
    //   • The player is grounded OR within the coyote-time grace window.
    bool canJump = onGround || (coyoteTimer > 0);
    if (jumpBuffer > 0 && canJump)
    {
        vel.y         = JUMP_FORCE;   // Initial upward impulse.
        onGround      = false;
        coyoteTimer   = 0;            // Consume coyote time so it can't fire again.
        jumpBuffer    = 0;            // Consume the buffered intent.
        jumpHeld      = true;
        jumpHoldTimer = 0;
        jmp           = true;
    }

    // ── Variable-height jump (hold bonus) ────────────────────────────────────
    // While Space remains held AND the player is still moving upward AND the
    // hold timer hasn't expired, add a small extra upward force each frame.
    // This gives the "tap for small jump, hold for big jump" feel.
    if (jumpHeld)
    {
        if (IsKeyDown(KEY_SPACE) && jumpHoldTimer < JUMP_HOLD_MAX && vel.y > 0)
        {
            vel.y         += JUMP_HOLD_FORCE * dt;
            jumpHoldTimer += dt;
        }
        else
        {
            // Space released, peak reached, or hold cap hit — stop the bonus.
            jumpHeld = false;
        }
    }

    // ── Horizontal movement ───────────────────────────────────────────────────

    if (shape == ShapeType::Cube)
    {
        // Cube: 1-D movement along X only.
        // Lerp toward the target speed (±CUBE_MAX or 0) using a high
        // acceleration factor → snappy, immediate-feeling control.
        float acc = (fabsf(rawDirX) > 0.01f) ? CUBE_ACC : CUBE_DEC;
        vel.x = Lerp(vel.x, rawDirX * CUBE_MAX, acc * dt);
        vel.z = 0.f;   // Z is always locked in 2-D mode.
    }
    else
    {
        // Sphere: 3-D movement on the XZ plane.
        // Use Clamp instead of Lerp so acceleration is applied as a
        // frame-rate-independent delta rather than a proportional lerp;
        // this prevents the sphere from instantly reaching max speed.
        if (Vector3Length(sphereDir) > 0.01f)
        {
            // Nudge current velocity toward (sphereDir × SPH_MAX) by at most
            // SPH_ACC * dt units/s per frame — gradual, weighty feel.
            vel.x += Clamp(sphereDir.x * SPH_MAX - vel.x, -SPH_ACC * dt, SPH_ACC * dt);
            vel.z += Clamp(sphereDir.z * SPH_MAX - vel.z, -SPH_ACC * dt, SPH_ACC * dt);
        }
        else
        {
            // No input → apply friction: lerp XZ velocity toward zero.
            vel.x = Lerp(vel.x, 0, SPH_DEC * dt);
            vel.z = Lerp(vel.z, 0, SPH_DEC * dt);
        }

        // Hard-cap horizontal speed so diagonal movement isn't faster than
        // cardinal movement (avoids the diagonal speed bug).
        float h = sqrtf(vel.x * vel.x + vel.z * vel.z);
        if (h > SPH_MAX) { float sc = SPH_MAX / h; vel.x *= sc; vel.z *= sc; }
    }
}

// =============================================================================
//  4. Update — integrate physics and age the trail
// =============================================================================
//
//  Call order each frame (from main.cpp):
//    HandleInput → Update → [collision in main] → Draw
//
//  Update ticks all timers, integrates vel into pos, and updates the
//  rolling animation.  Collision is NOT handled here because the platform
//  list lives in main.cpp; OnLand() is called from there after resolution.

void Player::Update(float dt)
{
    // Tick countdown timers — clamp to zero to avoid going negative.
    if (jumpBuffer > 0)    jumpBuffer  -= dt;
    if (landFlash  > 0)    landFlash   -= dt * 3.0f;   // Flash decays 3× faster than real-time.
    if (!onGround && coyoteTimer > 0) coyoteTimer -= dt;

    // ── Gravity ──────────────────────────────────────────────────────────────
    // Apply constant downward acceleration.  Terminal velocity of −32 units/s
    // prevents tunnelling through thin platforms at high fall speeds.
    vel.y -= GRAVITY * dt;
    if (vel.y < -32) vel.y = -32;

    // ── Euler integration ────────────────────────────────────────────────────
    // Simple forward-Euler: new_pos = old_pos + vel × dt.
    // Good enough for this game's physics fidelity requirements.
    pos = Vector3Add(pos, Vector3Scale(vel, dt));

    // ── Shape morph animation ─────────────────────────────────────────────────
    // morphT smoothly follows the actual shape (0 = cube, 1 = sphere).
    // Draw() interpolates geometry dimensions using this value.
    morphT = Lerp(morphT, (shape == ShapeType::Sphere) ? 1.f : 0.f, morphSpeed * dt);

    // ── Sphere rolling animation ──────────────────────────────────────────────
    if (shape == ShapeType::Sphere)
    {
        Vector3 hv  = { vel.x, 0, vel.z };   // Horizontal velocity only.
        float   spd = Vector3Length(hv);

        if (spd > 0.05f)
        {
            // Roll axis is the horizontal axis perpendicular to the movement
            // direction (cross product of world-up and normalised velocity).
            rollAxis  = Vector3Normalize(
                Vector3CrossProduct({ 0, 1, 0 }, Vector3Normalize(hv)));

            // Angular velocity = linear speed / radius  (rolling without slipping).
            rollAngle += (spd * dt) / radius;
        }

        // Sample trail position on a fixed timer.
        trailTimer -= dt;
        if (trailTimer <= 0) { AddTrailSample(); trailTimer = TRAIL_SAMPLE; }
    }

    // ── Age all trail samples ─────────────────────────────────────────────────
    for (int i = 0; i < trailCount; i++)
    {
        int idx = (trailHead - 1 - i + TRAIL_MAX) % TRAIL_MAX;
        trail[idx].age   += dt;
        // Alpha fades from 1 (fresh) to 0 over 0.42 seconds.
        trail[idx].alpha  = Clamp(1.f - trail[idx].age / 0.42f, 0.f, 1.f);
    }

    // Remove fully-faded samples from the oldest end of the ring buffer
    // (they are at the front, since trailHead points to the newest slot).
    while (trailCount > 0)
    {
        int oldest = (trailHead - trailCount + TRAIL_MAX) % TRAIL_MAX;
        if (trail[oldest].alpha <= 0) trailCount--;
        else break;
    }

    // Reset onGround every frame — it is re-set by collision resolution
    // in main.cpp if the player is actually touching a floor this frame.
    onGround = false;
}

// =============================================================================
//  5. OnLand — response when collision resolver detects a floor contact
// =============================================================================
//
//  Called from main.cpp immediately after Resolve() confirms the player is
//  resting on a surface (h.n.y > 0.7 and was moving downward).

void Player::OnLand(float surfY, float impact)
{
    // Half-height of the collision volume (radius for sphere, half-height for cube).
    float half = (shape == ShapeType::Sphere) ? radius : size.y * 0.5f;

    // Snap position so the bottom of the volume sits exactly on the surface.
    // This prevents the player from sinking into or floating above the platform
    // due to floating-point accumulation.
    pos.y = surfY + half;

    vel.y    = 0;      // Stop vertical movement.
    onGround = true;   // Re-grant ground state.

    // Reset coyote-time grace window so the player can jump again.
    coyoteTimer = COYOTE_TIME;

    // Store impact intensity for the landing flash visual (0–1 range).
    // Impact is capped at 18 units/s for normalisation; heavier landings
    // produce a stronger glow ring.
    landFlash = Clamp(impact / 18.f, 0.f, 1.f);
}

// =============================================================================
//  6. Particle spawning helpers
// =============================================================================

// ---------------------------------------------------------------------------
//  SpawnLandParticles — dust burst on heavy landing
// ---------------------------------------------------------------------------
//
//  Scales particle count and ejection speed with impact velocity.
//  Particles are radially scattered around the base of the player on the XZ
//  plane, with a small upward component so they arc visually.

void Player::SpawnLandParticles(std::vector<Particle>& pts, float impact) const
{
    if (impact < 1.5f) return;   // Ignore soft landings — no visual feedback needed.

    int n = (int)(impact * 2.5f);
    if (n > 14) n = 14;          // Hard cap to keep frame-time spike bounded.

    Color dc = { 210, 190, 155, 215 };   // Dusty beige colour.

    // Base Y: bottom of the player volume (just above the surface).
    float by = pos.y - (shape == ShapeType::Sphere ? radius : size.y * 0.5f) + 0.06f;

    for (int i = 0; i < n; i++)
    {
        // Random azimuth angle for radial spread.
        float a  = GetRandomValue(0, 628) * 0.01f;
        float sp = (float)GetRandomValue(2, 6);

        Particle p;
        // Start slightly off-centre to avoid a perfectly symmetric look.
        p.pos   = { pos.x + cosf(a) * 0.25f, by, pos.z + sinf(a) * 0.25f };
        p.vel   = { cosf(a) * sp, (float)GetRandomValue(2, 5), sinf(a) * sp };
        p.life  = p.maxLife = 0.4f + GetRandomValue(0, 15) * 0.01f;
        p.col   = dc;
        p.size  = 0.11f + GetRandomValue(0, 8) * 0.01f;
        pts.push_back(p);
    }
}

// ---------------------------------------------------------------------------
//  SpawnCollectParticles — coloured radial burst (checkpoints / switches)
// ---------------------------------------------------------------------------
//
//  Exactly 20 particles evenly distributed around a full sphere using
//  azimuth + elevation angles, giving a consistent "starburst" look.

void Player::SpawnCollectParticles(std::vector<Particle>& pts, Color col) const
{
    for (int i = 0; i < 20; i++)
    {
        // Evenly space azimuth angles across a full circle.
        float a  = (float)i / 20.f * 6.2831853f;
        // Random elevation so particles scatter in 3-D, not just on XZ plane.
        float el = GetRandomValue(-25, 65) * 0.01745f;   // −25° to +65° in radians.
        float sp = (float)GetRandomValue(4, 10);

        Particle p;
        p.pos = pos;
        // Convert spherical coordinates to Cartesian velocity.
        p.vel = {
            cosf(a) * cosf(el) * sp,
            sinf(el) * sp + 2.f,      // +2 bias so particles arc upward overall.
            sinf(a) * cosf(el) * sp
        };
        p.life = p.maxLife = 0.5f + GetRandomValue(0, 25) * 0.01f;
        p.col  = col;
        p.size = 0.14f + GetRandomValue(0, 12) * 0.01f;
        pts.push_back(p);
    }
}

// =============================================================================
//  7. AddTrailSample (private)  — write current position into the ring buffer
// =============================================================================

void Player::AddTrailSample()
{
    // Write the new sample at trailHead, then advance the write pointer.
    trail[trailHead] = { pos, 0, 1 };
    trailHead = (trailHead + 1) % TRAIL_MAX;   // Wrap around using modulo.

    // Only increment count until the buffer is full; after that, the oldest
    // sample is silently overwritten (the ring buffer's intended behaviour).
    if (trailCount < TRAIL_MAX) trailCount++;
}

// =============================================================================
//  8. Draw  — renders the player and its motion trail
// =============================================================================
//
//  The draw order is:
//    a) Trail ghost spheres (behind the player in depth)
//    b) Main player geometry (cube or sphere, depending on current shape)
//    c) Landing flash / glow ring overlaid on the player
//    d) Ground shadow blob
//
//  morphT blends dimensions between cube and sphere during shape transitions
//  so neither shape pops in abruptly.

void Player::Draw() const
{
    // ─────────────────────────────────────────────────────────────────────────
    //  a) Motion trail  (only visible in Sphere mode; fades out on morph)
    // ─────────────────────────────────────────────────────────────────────────
    if (morphT > 0.05f)
    {
        for (int i = 0; i < trailCount; i++)
        {
            // Walk the ring buffer from newest (i=0) to oldest (i=trailCount−1).
            int idx = (trailHead - 1 - i + TRAIL_MAX) % TRAIL_MAX;
            const auto& s = trail[idx];
            if (s.alpha <= 0.01f) continue;

            // Older samples (larger i) → smaller radius and dimmer colour.
            float frac = (float)(i + 1) / TRAIL_MAX;
            float r    = radius * Lerp(0.38f, 0.02f, frac) * morphT;

            // Bright icy-blue tint that fades with age.
            unsigned char bv  = (unsigned char)(200 * s.alpha * (1 - frac * 0.6f));
            unsigned char bv2 = (unsigned char)(bv < 205 ? bv + 50 : 255);
            unsigned char al  = (unsigned char)(175 * s.alpha * morphT);

            DrawSphere(s.pos, r, { bv, bv2, 255, al });
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  b) Main player geometry
    // ─────────────────────────────────────────────────────────────────────────
    if (shape == ShapeType::Sphere)
    {
        // Y position of the sphere's lowest point (used for shadow & glow ring).
        float groundY = pos.y - radius + 0.04f;

        // Ambient glow ring at the base of the sphere.
        DrawCircle3D({ pos.x, groundY, pos.z }, radius * 1.12f,
                     { 1, 0, 0 }, 90,
                     { 80, 160, 255, (unsigned char)(35 + landFlash * 55) });

        // Push an isolated transform so the rolling rotation doesn't leak into
        // other draw calls.
        rlPushMatrix();
            rlTranslatef(pos.x, pos.y, pos.z);
            rlRotatef(rollAngle * RAD2DEG, rollAxis.x, rollAxis.y, rollAxis.z);

            // Outer shell — opaque white-blue, high poly count for smooth look.
            DrawSphereEx({ 0, 0, 0 }, radius, 28, 28, { 228, 244, 255, 255 });

            // Inner glow layer — semi-transparent overlay gives depth.
            DrawSphereEx({ 0, 0, 0 }, radius * 0.80f, 16, 16, { 160, 205, 255, 50 });

            // Specular highlight — small bright spot offset from centre.
            DrawSphereEx({ radius * 0.22f, radius * 0.30f, radius * 0.52f },
                         radius * 0.23f, 8, 8, { 255, 255, 255, 255 });

            // Wireframe overlay — subtle grid that rotates with the ball,
            // reinforcing the rolling-sphere visual effect.
            DrawSphereWires({ 0, 0, 0 }, radius, 9, 9, { 50, 120, 255, 40 });
        rlPopMatrix();

        // ── Landing flash ring ───────────────────────────────────────────────
        // An expanding circle drawn at ground level that quickly fades.
        if (landFlash > 0)
        {
            float ex = (1 - landFlash) * 2.8f;   // Expansion factor grows as flash fades.
            DrawCircle3D({ pos.x, groundY, pos.z }, radius * (1 + ex),
                         { 1, 0, 0 }, 90,
                         { 180, 220, 255, (unsigned char)(landFlash * 210) });
        }

        // ── Soft ground shadow blob ──────────────────────────────────────────
        // Opacity and scale shrink as the player rises above the floor,
        // giving a cheap contact-shadow without ray-casting.
        float sa = Clamp(1.f - (pos.y - radius) * 0.09f, 0.f, 0.60f);
        DrawCircle3D({ pos.x, -0.98f, pos.z }, radius * 0.80f * sa,
                     { 1, 0, 0 }, 90,
                     { 0, 0, 0, (unsigned char)(95 * sa) });
    }
    else
    {
        // ── Cube rendering ───────────────────────────────────────────────────
        // Dimensions lerp between the cube's base size and the sphere's
        // bounding dimensions during the morph transition (morphT 0→1).
        float bH = Lerp(2.0f, radius * 2.f, morphT);   // Height.
        float bW = Lerp(1.0f, radius * 2.f, morphT);   // Width and depth.

        rlPushMatrix();
            rlTranslatef(pos.x, pos.y, pos.z);
            rlScalef(bW, bH, bW);   // Non-uniform scale for width vs height.

            // Main body — vivid red.
            DrawCube({ 0, 0, 0 }, 1, 1, 1, { 218, 45, 45, 255 });

            // Top face highlight — brighter stripe to suggest a lit top surface.
            DrawCube({ 0, 0.47f, 0 }, 1, 0.08f, 1, { 255, 95, 75, 255 });

            // Front face highlight — slight brightening for a fake rim-light.
            DrawCube({ 0, 0, 0.47f }, 1, 1, 0.08f, { 238, 68, 55, 255 });

            // Right face darkening — simulate directional shadow on the side.
            DrawCube({ 0.47f, 0, 0 }, 0.08f, 1, 1, { 200, 38, 38, 255 });

            // Wireframe outline — black with slight transparency for a
            // chunky cartoon border look.
            DrawCubeWires({ 0, 0, 0 }, 1.02f, 1.02f, 1.02f, { 0, 0, 0, 175 });
        rlPopMatrix();

        // ── Landing flash (cube) ─────────────────────────────────────────────
        // A flat shockwave slab that briefly appears beneath the cube,
        // expanding outward as the flash fades.
        if (landFlash > 0)
        {
            float fw = bW + (1 - landFlash) * 1.8f;
            DrawCube({ pos.x, pos.y - bH * 0.5f + 0.05f, pos.z },
                     fw, 0.07f, fw,
                     { 255, 175, 55, (unsigned char)(landFlash * 185) });
        }
    }
}