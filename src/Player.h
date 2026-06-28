#pragma once
#include "raylib.h"
#include "raymath.h"
#include <vector>

enum class ShapeType { Cube, Sphere };

struct TrailSample { Vector3 pos; float age; float alpha; };

struct Particle { Vector3 pos, vel; float life, maxLife; Color col; float size; };

class Player
{
public:

    Vector3   pos;
    Vector3   vel;
    Vector3   size;
    bool      onGround;
    ShapeType shape;

    float   radius;
    float   rollAngle;
    Vector3 rollAxis;

    float morphBlend, morphSpeed;

    static constexpr float COYOTE_TIME = 0.14f;

    static constexpr float JUMP_BUFFER_TIME = 0.16f;

    bool  doubleJumpAvailable;

    float coyoteTimer, jumpBuffer;
    float jumpHoldTimer;
    bool  jumpHeld;
    static constexpr float JUMP_HOLD_MAX   = 0.26f;
    static constexpr float JUMP_HOLD_FORCE = 18.f;

    static constexpr int   TRAIL_MAX    = 32;
    static constexpr float TRAIL_SAMPLE = 0.025f;

    TrailSample trail[TRAIL_MAX];
    int   trailHead, trailCount;
    float trailTimer;

    float landFlash;

    Player();

    void HandleInput(float dt, float rawDirX, Vector3 sphereDir,
                     bool& outJumped, bool& outSwitched);

    void Update(float dt);

    void Draw() const;

    void OnLand(float surfaceY, float impact);

    void SpawnLandParticles(std::vector<Particle>& pts, float impact) const;

    void SpawnCollectParticles(std::vector<Particle>& pts, Color col) const;

private:

    void AddTrailSample();
};