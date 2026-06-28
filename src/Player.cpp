#include "Player.h"
#include "raymath.h"
#include "rlgl.h"
#include <cmath>
#include <vector>

static constexpr float GRAVITY    = 26.f;
static constexpr float JUMP_FORCE = 10.5f;

static constexpr float CUBE_MAX = 9.0f,  CUBE_ACC = 58.f, CUBE_DEC = 70.f;
static constexpr float SPH_MAX  = 11.5f, SPH_ACC  = 9.8f, SPH_DEC  = 2.5f;

Player::Player()
{

    pos = { 0, 3, 0 };
    vel = { 0, 0, 0 };

    size = { 1, 2, 1 };

    onGround = false;

    radius    = 0.85f;
    rollAngle = 0;
    rollAxis  = { 1, 0, 0 };

    shape     = ShapeType::Cube;

    morphBlend     = 0;
    morphSpeed = 8;

    coyoteTimer  = 0;
    jumpBuffer   = 0;
    jumpHoldTimer = 0;
    jumpHeld     = false;

    trailHead  = 0;
    trailCount = 0;
    trailTimer = 0;

    landFlash = 0;
}

void Player::HandleInput(float dt, float rawDirX, Vector3 sphereDir,
                         bool& jmp, bool& sw)
{
    jmp = sw = false;

    if (IsKeyPressed(KEY_ONE) && shape != ShapeType::Cube)
    {
        shape  = ShapeType::Cube;
        size   = { 1, 2, 1 };
        vel.z  = 0;
        sw     = true;
    }

    if (IsKeyPressed(KEY_TWO) && shape != ShapeType::Sphere)
    {
        shape  = ShapeType::Sphere;
        radius = 0.85f;
        size   = { 1.7f, 1.7f, 1.7f };
        sw     = true;
    }

    if (IsKeyPressed(KEY_TAB)) ToggleFullscreen();

    if (IsKeyPressed(KEY_SPACE)) jumpBuffer = JUMP_BUFFER_TIME;

    bool canJump = onGround || (coyoteTimer > 0);
    if (jumpBuffer > 0 && canJump)
    {
        vel.y         = JUMP_FORCE;
        onGround      = false;
        coyoteTimer   = 0;
        jumpBuffer    = 0;
        jumpHeld      = true;
        jumpHoldTimer = 0;
        jmp           = true;
    }

    if (jumpHeld)
    {
        if (IsKeyDown(KEY_SPACE) && jumpHoldTimer < JUMP_HOLD_MAX && vel.y > 0)
        {
            vel.y         += JUMP_HOLD_FORCE * dt;
            jumpHoldTimer += dt;
        }
        else
        {

            jumpHeld = false;
        }
    }

    if (shape == ShapeType::Cube)
    {

        float acc = (fabsf(rawDirX) > 0.01f) ? CUBE_ACC : CUBE_DEC;
        vel.x = Lerp(vel.x, rawDirX * CUBE_MAX, acc * dt);
        vel.z = 0.f;
    }
    else
    {

        if (Vector3Length(sphereDir) > 0.01f)
        {

            vel.x += Clamp(sphereDir.x * SPH_MAX - vel.x, -SPH_ACC * dt, SPH_ACC * dt);
            vel.z += Clamp(sphereDir.z * SPH_MAX - vel.z, -SPH_ACC * dt, SPH_ACC * dt);
        }
        else
        {

            vel.x = Lerp(vel.x, 0, SPH_DEC * dt);
            vel.z = Lerp(vel.z, 0, SPH_DEC * dt);
        }

        float h = sqrtf(vel.x * vel.x + vel.z * vel.z);
        if (h > SPH_MAX) { float sc = SPH_MAX / h; vel.x *= sc; vel.z *= sc; }
    }
}

void Player::Update(float dt)
{

    if (jumpBuffer > 0)    jumpBuffer  -= dt;
    if (landFlash  > 0)    landFlash   -= dt * 3.0f;
    if (!onGround && coyoteTimer > 0) coyoteTimer -= dt;

    vel.y -= GRAVITY * dt;
    if (vel.y < -32) vel.y = -32;

    pos = Vector3Add(pos, Vector3Scale(vel, dt));

    morphBlend = Lerp(morphBlend, (shape == ShapeType::Sphere) ? 1.f : 0.f, morphSpeed * dt);

    if (shape == ShapeType::Sphere)
    {
        Vector3 hv  = { vel.x, 0, vel.z };
        float   spd = Vector3Length(hv);

        if (spd > 0.05f)
        {

            rollAxis  = Vector3Normalize(
                Vector3CrossProduct({ 0, 1, 0 }, Vector3Normalize(hv)));

            rollAngle += (spd * dt) / radius;
        }

        trailTimer -= dt;
        if (trailTimer <= 0) { AddTrailSample(); trailTimer = TRAIL_SAMPLE; }
    }

    for (int i = 0; i < trailCount; i++)
    {
        int idx = (trailHead - 1 - i + TRAIL_MAX) % TRAIL_MAX;
        trail[idx].age   += dt;

        trail[idx].alpha  = Clamp(1.f - trail[idx].age / 0.42f, 0.f, 1.f);
    }

    while (trailCount > 0)
    {
        int oldest = (trailHead - trailCount + TRAIL_MAX) % TRAIL_MAX;
        if (trail[oldest].alpha <= 0) trailCount--;
        else break;
    }

    onGround = false;
}

void Player::OnLand(float surfY, float impact)
{

    float half = (shape == ShapeType::Sphere) ? radius : size.y * 0.5f;

    pos.y = surfY + half;

    vel.y    = 0;
    onGround = true;

    coyoteTimer = COYOTE_TIME;

    landFlash = Clamp(impact / 18.f, 0.f, 1.f);
}

void Player::SpawnLandParticles(std::vector<Particle>& pts, float impact) const
{
    if (impact < 1.5f) return;

    int n = (int)(impact * 2.5f);
    if (n > 14) n = 14;

    Color dc = { 210, 190, 155, 215 };

    float by = pos.y - (shape == ShapeType::Sphere ? radius : size.y * 0.5f) + 0.06f;

    for (int i = 0; i < n; i++)
    {

        float a  = GetRandomValue(0, 628) * 0.01f;
        float sp = (float)GetRandomValue(2, 6);

        Particle p;

        p.pos   = { pos.x + cosf(a) * 0.25f, by, pos.z + sinf(a) * 0.25f };
        p.vel   = { cosf(a) * sp, (float)GetRandomValue(2, 5), sinf(a) * sp };
        p.life  = p.maxLife = 0.4f + GetRandomValue(0, 15) * 0.01f;
        p.col   = dc;
        p.size  = 0.11f + GetRandomValue(0, 8) * 0.01f;
        pts.push_back(p);
    }
}

void Player::SpawnCollectParticles(std::vector<Particle>& pts, Color col) const
{
    for (int i = 0; i < 20; i++)
    {

        float a  = (float)i / 20.f * 6.2831853f;

        float el = GetRandomValue(-25, 65) * 0.01745f;
        float sp = (float)GetRandomValue(4, 10);

        Particle p;
        p.pos = pos;

        p.vel = {
            cosf(a) * cosf(el) * sp,
            sinf(el) * sp + 2.f,
            sinf(a) * cosf(el) * sp
        };
        p.life = p.maxLife = 0.5f + GetRandomValue(0, 25) * 0.01f;
        p.col  = col;
        p.size = 0.14f + GetRandomValue(0, 12) * 0.01f;
        pts.push_back(p);
    }
}

void Player::AddTrailSample()
{

    trail[trailHead] = { pos, 0, 1 };
    trailHead = (trailHead + 1) % TRAIL_MAX;

    if (trailCount < TRAIL_MAX) trailCount++;
}

void Player::Draw() const
{

    if (morphBlend > 0.05f)
    {
        for (int i = 0; i < trailCount; i++)
        {

            int idx = (trailHead - 1 - i + TRAIL_MAX) % TRAIL_MAX;
            const auto& s = trail[idx];
            if (s.alpha <= 0.01f) continue;

            float frac = (float)(i + 1) / TRAIL_MAX;
            float r    = radius * Lerp(0.38f, 0.02f, frac) * morphBlend;

            unsigned char blue   = (unsigned char)(200 * s.alpha * (1 - frac * 0.6f));
            unsigned char blueBright  = (unsigned char)(blue  < 205 ? blue  + 50 : 255);
            unsigned char alpha  = (unsigned char)(175 * s.alpha * morphBlend);

            DrawSphere(s.pos, r, { blue , blueBright , 255, alpha });
        }
    }

    if (shape == ShapeType::Sphere)
    {

        float groundY = pos.y - radius + 0.04f;

        DrawCircle3D({ pos.x, groundY, pos.z }, radius * 1.12f,
                     { 1, 0, 0 }, 90,
                     { 80, 160, 255, (unsigned char)(35 + landFlash * 55) });

        rlPushMatrix();
            rlTranslatef(pos.x, pos.y, pos.z);
            rlRotatef(rollAngle * RAD2DEG, rollAxis.x, rollAxis.y, rollAxis.z);

            DrawSphereEx({ 0, 0, 0 }, radius, 28, 28, { 228, 244, 255, 255 });

            DrawSphereEx({ 0, 0, 0 }, radius * 0.80f, 16, 16, { 160, 205, 255, 50 });

            DrawSphereEx({ radius * 0.22f, radius * 0.30f, radius * 0.52f },
                         radius * 0.23f, 8, 8, { 255, 255, 255, 255 });

            DrawSphereWires({ 0, 0, 0 }, radius, 9, 9, { 50, 120, 255, 40 });
        rlPopMatrix();

        if (landFlash > 0)
        {
            float ex = (1 - landFlash) * 2.8f;
            DrawCircle3D({ pos.x, groundY, pos.z }, radius * (1 + ex),
                         { 1, 0, 0 }, 90,
                         { 180, 220, 255, (unsigned char)(landFlash * 210) });
        }

        float sa = Clamp(1.f - (pos.y - radius) * 0.09f, 0.f, 0.60f);
        DrawCircle3D({ pos.x, -0.98f, pos.z }, radius * 0.80f * sa,
                     { 1, 0, 0 }, 90,
                     { 0, 0, 0, (unsigned char)(95 * sa) });
    }
    else
    {

        float boxHeight = Lerp(2.0f, radius * 2.f, morphBlend);
        float boxWidth = Lerp(1.0f, radius * 2.f, morphBlend);

        rlPushMatrix();
            rlTranslatef(pos.x, pos.y, pos.z);
            rlScalef(boxWidth, boxHeight, boxWidth);

            DrawCube({ 0, 0, 0 }, 1, 1, 1, { 218, 45, 45, 255 });

            DrawCube({ 0, 0.47f, 0 }, 1, 0.08f, 1, { 255, 95, 75, 255 });

            DrawCube({ 0, 0, 0.47f }, 1, 1, 0.08f, { 238, 68, 55, 255 });

            DrawCube({ 0.47f, 0, 0 }, 0.08f, 1, 1, { 200, 38, 38, 255 });

            DrawCubeWires({ 0, 0, 0 }, 1.02f, 1.02f, 1.02f, { 0, 0, 0, 175 });
        rlPopMatrix();

        if (landFlash > 0)
        {
            float flashWidth = boxWidth + (1 - landFlash) * 1.8f;
            DrawCube({ pos.x, pos.y - boxHeight * 0.5f + 0.05f, pos.z },
                     flashWidth, 0.07f, flashWidth,
                     { 255, 175, 55, (unsigned char)(landFlash * 185) });
        }
    }
}