#pragma once

#include "CoreMinimal.h"

// M1-029: per-axis planar movement speeds with diagonal input normalization.
// Axis convention (project interface doc, space section): X horizontal,
// Y depth, Z vertical. Facing mirrors X only.

namespace UE::UEMMO::Tasks::M1_029
{
    // Per-axis walk speeds in cm/s; horizontal (X) is faster than depth (Y).
    constexpr float PlanarSpeedX = 420.0f;
    constexpr float PlanarSpeedY = 280.0f;

    /** Accumulated raw planar axis input owned by the character. */
    struct FPlanarAxisState
    {
        float AxisX = 0.0f; // -1..1; +1 = right (D), -1 = left (A)
        float AxisY = 0.0f; // -1..1; depth axis (S positive, W negative)

        // Opposite contributions on the same axis cancel: +1 then -1 sums to 0.
        void AddAxisX(float Value) { AxisX = SanitizeAxis(AxisX + Value); }
        void AddAxisY(float Value) { AxisY = SanitizeAxis(AxisY + Value); }
        // Enhanced Input already sums all keys mapped to one action; assign the net value.
        void SetAxisX(float Value) { AxisX = SanitizeAxis(Value); }
        void SetAxisY(float Value) { AxisY = SanitizeAxis(Value); }

        // Out-of-range values clamp to [-1,1]; non-finite input counts as 0.
        static float SanitizeAxis(float Value)
        {
            return FMath::IsFinite(Value) ? FMath::Clamp(Value, -1.0f, 1.0f) : 0.0f;
        }
    };

    /**
     * Planar velocity (cm/s) for a raw 2D axis pair in [-1,1].
     * The input vector is normalized first (so diagonal input is not two axes
     * at full speed), then scaled by the per-axis speeds. Zero input yields a
     * zero vector; non-finite components count as 0; others clamp to [-1,1].
     */
    inline FVector ComputePlanarVelocity(float AxisX, float AxisY, float SpeedX = PlanarSpeedX, float SpeedY = PlanarSpeedY)
    {
        AxisX = FPlanarAxisState::SanitizeAxis(AxisX);
        AxisY = FPlanarAxisState::SanitizeAxis(AxisY);
        const float SizeSquared = AxisX * AxisX + AxisY * AxisY;
        if (SizeSquared <= UE_SMALL_NUMBER)
        {
            return FVector::ZeroVector;
        }
        const float InvSize = FMath::InvSqrt(SizeSquared);
        return FVector(AxisX * InvSize * SpeedX, AxisY * InvSize * SpeedY, 0.0f);
    }

    inline FVector ComputePlanarVelocity(const FPlanarAxisState& Axes, float SpeedX = PlanarSpeedX, float SpeedY = PlanarSpeedY)
    {
        return ComputePlanarVelocity(Axes.AxisX, Axes.AxisY, SpeedX, SpeedY);
    }
}
