// =============================================================================
//  Player.h  —  Player entity declaration for ORBICUBE
// =============================================================================
//
//  The Player can exist in two shapes that fundamentally change both movement
//  feel and the set of platforms it can interact with:
//
//    • ShapeType::Cube   — 2D side-scroller mode.  Movement is axis-locked to
//                          X, gravity pulls down Y, Z is forced to 0.
//                          Sharp acceleration / deceleration; jumps are precise.
//
//    • ShapeType::Sphere — full 3D mode.  Movement is camera-relative on the
//                          XZ plane; floaty, physics-heavy feel with a visible
//                          rolling animation and a motion-blur trail.
//
//  Switching shape is instant in terms of game-state but the visual transition
//  is driven by `morphT`, a 0-to-1 value that lerps the drawn geometry.
//
// =============================================================================

#pragma once
#include "raylib.h"
#include "raymath.h"
#include <vector>

// ---------------------------------------------------------------------------
//  Shape enum
// ---------------------------------------------------------------------------

/** Selects which movement model and collision geometry the player uses. */
enum class ShapeType { Cube, Sphere };

// ---------------------------------------------------------------------------
//  Helper POD structs
// ---------------------------------------------------------------------------

/**
 * One sample of the sphere's motion trail.
 *
 * Samples are stored in a fixed-size ring buffer (`trail[]`) and are added
 * every TRAIL_SAMPLE seconds while the player is in Sphere mode.
 * `age`   — seconds since this sample was captured; drives fade-out.
 * `alpha` — pre-computed opacity [1→0] used directly when drawing.
 */
struct TrailSample { Vector3 pos; float age; float alpha; };

/**
 * A single visual-only particle (no collision).
 *
 * Particles are owned by the global `gParts` vector in main.cpp, not by the
 * Player itself.  The Player's spawn helpers just push_back into that vector.
 *
 * `life / maxLife` — used to compute alpha fade and scale shrink over time.
 * `size`           — base radius of the sphere drawn for this particle.
 */
struct Particle { Vector3 pos, vel; float life, maxLife; Color col; float size; };

// =============================================================================
//  Player class
// =============================================================================

class Player
{
public:
    // -------------------------------------------------------------------------
    //  Core physics state
    // -------------------------------------------------------------------------

    Vector3   pos;        ///< World-space centre of the player's bounding volume.
    Vector3   vel;        ///< Linear velocity in units/second (X, Y, Z).
    Vector3   size;       ///< Axis-aligned bounding box dimensions (used for Cube).
    bool      onGround;   ///< True if the player was resolved against a floor this frame.
    ShapeType shape;      ///< Current active shape (drives physics + collision mode).

    // -------------------------------------------------------------------------
    //  Sphere-specific state
    // -------------------------------------------------------------------------

    float   radius;       ///< Collision/render radius when in Sphere mode.
    float   rollAngle;    ///< Accumulated rotation angle (radians) for the rolling animation.
    Vector3 rollAxis;     ///< Axis around which the sphere visually rolls (derived from velocity).

    // -------------------------------------------------------------------------
    //  Shape-morph transition
    // -------------------------------------------------------------------------

    /**
     * morphT — smooth blend between Cube (0) and Sphere (1).
     *
     * Updated every frame via Lerp toward the target shape value so that
     * the geometry visually crossfades rather than snapping.
     * morphSpeed controls how fast the lerp converges (higher = snappier).
     */
    float morphT, morphSpeed;

    // -------------------------------------------------------------------------
    //  Jump system constants
    // -------------------------------------------------------------------------

    /**
     * COYOTE_TIME — "coyote time" grace window.
     *
     * After walking off a ledge the player can still jump for this many seconds.
     * This is a standard platformer feel technique that prevents frustrating
     * missed jumps at ledge edges.
     */
    static constexpr float COYOTE_TIME = 0.14f;

    /**
     * JUMP_BUFFER_TIME — jump-input buffer window.
     *
     * If the player presses Space up to this many seconds *before* landing,
     * the jump is queued and fires automatically on the next grounded frame.
     * Prevents missed jumps due to slightly early input.
     */
    static constexpr float JUMP_BUFFER_TIME = 0.16f;

    // -------------------------------------------------------------------------
    //  Jump runtime state
    // -------------------------------------------------------------------------

    bool  doubleJumpAvailable; ///< Reserved for a future double-jump mechanic (currently unused).

    /**
     * Variable-height jumping via "hold to jump higher".
     *
     * jumpBuffer     — countdown timer set to JUMP_BUFFER_TIME on Space press;
     *                  fires the jump when it is still > 0 and player is grounded.
     * jumpHeld       — true while Space is held after the jump was initiated.
     * jumpHoldTimer  — accumulates hold duration; capped at JUMP_HOLD_MAX so
     *                  the bonus force cannot be applied indefinitely.
     * JUMP_HOLD_FORCE — extra upward acceleration per second while holding.
     */
    float coyoteTimer, jumpBuffer;
    float jumpHoldTimer;
    bool  jumpHeld;
    static constexpr float JUMP_HOLD_MAX   = 0.26f;  ///< Max seconds of hold-jump boost.
    static constexpr float JUMP_HOLD_FORCE = 18.f;   ///< Extra upward accel (units/s²) while held.

    // -------------------------------------------------------------------------
    //  Motion trail (Sphere mode only)
    // -------------------------------------------------------------------------

    /**
     * Ring-buffer trail system for the sphere's ghost afterimage.
     *
     * TRAIL_MAX    — capacity of the ring buffer (number of stored samples).
     * TRAIL_SAMPLE — how often (seconds) a new sample is pushed.
     * trailHead    — index of the next write slot (wraps with modulo).
     * trailCount   — number of valid (not yet faded) samples currently stored.
     * trailTimer   — countdown to the next sample capture.
     */
    static constexpr int   TRAIL_MAX    = 32;
    static constexpr float TRAIL_SAMPLE = 0.025f;

    TrailSample trail[TRAIL_MAX];
    int   trailHead, trailCount;
    float trailTimer;

    // -------------------------------------------------------------------------
    //  Visual feedback
    // -------------------------------------------------------------------------

    /**
     * landFlash — 0-to-1 intensity of the landing impact flash/ring.
     *
     * Set by OnLand() proportional to impact speed, then decays to 0 each
     * frame in Update().  Draw() uses it to render an expanding glow circle.
     */
    float landFlash;

    // -------------------------------------------------------------------------
    //  Public interface
    // -------------------------------------------------------------------------

    /** Initialises all fields to safe defaults; placed at world origin. */
    Player();

    /**
     * Reads keyboard input and mutates velocity accordingly.
     *
     * Must be called once per frame BEFORE Update().
     *
     * @param dt          Delta-time in seconds.
     * @param rawDirX     Raw horizontal axis for Cube mode (-1, 0, or +1).
     * @param sphereDir   Camera-relative movement direction for Sphere mode
     *                    (length ≤ 1, already normalised by the caller).
     * @param outJumped   Set to true if a jump was initiated this frame
     *                    (used by main.cpp to trigger the jump sound).
     * @param outSwitched Set to true if the shape was changed this frame
     *                    (used by main.cpp to trigger the morph sound).
     */
    void HandleInput(float dt, float rawDirX, Vector3 sphereDir,
                     bool& outJumped, bool& outSwitched);

    /**
     * Integrates velocity into position, applies gravity, ages trail samples,
     * and ticks all cooldown timers.
     *
     * Collision resolution happens in main.cpp *after* this call, since the
     * platform list lives there.
     *
     * @param dt Delta-time in seconds.
     */
    void Update(float dt);

    /**
     * Renders the player (cube or sphere geometry) plus the motion trail.
     *
     * Does NOT push/pop the camera — assumes BeginMode3D() is already active.
     * Uses rlPushMatrix / rlPopMatrix internally for transforms.
     */
    void Draw() const;

    /**
     * Called by main.cpp's collision resolver when the player lands on a surface.
     *
     * Snaps the player flush with the surface, zeroes Y velocity, sets
     * `onGround`, resets coyote time, and records impact strength for landFlash.
     *
     * @param surfaceY  World Y of the top face of the platform just landed on.
     * @param impact    Speed (units/s) of the downward velocity just before landing.
     */
    void OnLand(float surfaceY, float impact);

    /**
     * Pushes dust/dirt particles into `pts` on a heavy landing.
     *
     * Particle count and speed scale with `impact`; capped at 14 particles to
     * avoid large frame-time spikes.
     */
    void SpawnLandParticles(std::vector<Particle>& pts, float impact) const;

    /**
     * Pushes a radial burst of coloured particles into `pts`.
     *
     * Used for checkpoint activation and switch interaction feedback.
     * Always spawns exactly 20 particles spread across a hemisphere.
     */
    void SpawnCollectParticles(std::vector<Particle>& pts, Color col) const;

private:
    /**
     * Writes the current position into the trail ring buffer and advances
     * `trailHead`.  Only called while in Sphere mode, on a fixed timer.
     */
    void AddTrailSample();
};