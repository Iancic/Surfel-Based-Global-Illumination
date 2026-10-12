#pragma once

#ifdef __cplusplus
#include "Math/Vector4.h"
#define LIGHT_VECTOR_4 FVector4f
#else
#define LIGHT_VECTOR_4 float4
#endif

struct FSurfelLightStruct
{
	LIGHT_VECTOR_4 PositionAndAttenuation; // attenuation for point light (w)
	LIGHT_VECTOR_4 ColorAndType; // type is (w)
	LIGHT_VECTOR_4 DirectionAndInverseFalloff; // dictates whether is inverse square or exponential falloff
	LIGHT_VECTOR_4 Angle;
};

#undef LIGHT_VECTOR_4
