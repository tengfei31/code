//
// Created by hujianzhe
//

#ifndef UTIL_C_SYSLIB_MATH_H
#define	UTIL_C_SYSLIB_MATH_H

#include "../compiler_define.h"

#ifndef	_USE_MATH_DEFINES
	#define	_USE_MATH_DEFINES
#endif
#include <float.h>
#include <math.h>
typedef	float		float32_t;
typedef double		float64_t;
#if REAL_FLOAT_BIT == 64
	typedef	float64_t					real_t;
	#define	REAL_EPSILON				DBL_EPSILON
	#define	REAL_MIN					DBL_MIN
	#define	REAL_MAX					DBL_MAX
	#define	REAL_MATH_FUNC(fn)			fn
#else
	#define	REAL_FLOAT_BIT				32
	typedef	float32_t					real_t;
	#define	REAL_EPSILON				FLT_EPSILON
	#define	REAL_MIN					FLT_MIN
	#define	REAL_MAX					FLT_MAX
	#define	REAL_MATH_FUNC(fn)			fn##f
#endif
#ifndef CCT_EPSILON
	#define	CCT_EPSILON					1E-5f
#endif

#ifdef	__cplusplus
extern "C" {
#endif

__declspec_dll int fcmpf(float a, float b, float epsilon);
__declspec_dll int fcmp(double a, double b, double epsilon);
__declspec_dll float finvsqrtf(float x);
__declspec_dll float fsqrtf(float x);
__declspec_dll double finvsqrt(double x);
__declspec_dll double fsqrt(double x);

#define	mathDegToRad(deg)	(M_PI / 180.0 * (deg))
#define	mathRadToDeg(rad)	((rad) * M_1_PI * 180.0)
#define	mathDegToRad_f(deg)	(((float)M_PI) / 180.0f * ((float)(deg)))
#define	mathRadToDeg_f(rad)	(((float)(rad)) * ((float)M_1_PI) * 180.0f)

__declspec_dll int mathQuadraticEquation(float a, float b, float c, float r[2]);

__declspec_dll int mathVec3IsZero(const float v[3]);
__declspec_dll int mathVec3Equal(const float v1[3], const float v2[3]);
__declspec_dll float* mathVec3Copy(float r[3], const float v[3]);
__declspec_dll float mathVec3LenSq(const float v[3]);
__declspec_dll float mathVec3Len(const float v[3]);
__declspec_dll float mathVec3Normalized(float r[3], const float v[3]);
__declspec_dll float* mathVec3Negate(float r[3], const float v[3]);
__declspec_dll float* mathVec3Add(float r[3], const float v1[3], const float v2[3]);
__declspec_dll float* mathVec3AddScalar(float r[3], const float v[3], float n);
__declspec_dll float* mathVec3Sub(float r[3], const float v1[3], const float v2[3]);
__declspec_dll float* mathVec3MultiplyScalar(float r[3], const float v[3], float n);
__declspec_dll float mathVec3Dot(const float v1[3], const float v2[3]);
__declspec_dll float mathVec3Radian(const float v1[3], const float v2[3]);
__declspec_dll float* mathVec3Cross(float r[3], const float v1[3], const float v2[3]);

__declspec_dll float* mathCoordinateSystemTransform(const float v[3], const float new_origin[3], float new_axies[3][3], float new_v[3]);

__declspec_dll float* mathQuatNormalized(float r[4], const float q[4]);
__declspec_dll float* mathQuatFromEuler(float q[4], const float e[3], const char order[3]);
__declspec_dll float* mathQuatFromUnitVec3(float q[4], const float from[3], const float to[3]);
__declspec_dll float* mathQuatFromAxisRadian(float q[4], const float axis[3], float radian);
__declspec_dll void mathQuatToAxisRadian(float q[4], float axis[3], float* radian);
__declspec_dll float* mathQuatIdentity(float q[4]);
__declspec_dll float* mathQuatConjugate(float r[4], const float q[4]);
__declspec_dll float* mathQuatMulQuat(float r[4], const float q1[4], const float q2[4]);
__declspec_dll float* mathQuatMulVec3(float r[3], const float q[4], const float v[3]);

__declspec_dll void mathPointProjectionLine(const float p[3], const float ls_v[3], const float lsdir[3], float vec_to_p[3], float np[3]);
__declspec_dll int mathLineClosestLine(const float lsv1[3], const float lsdir1[3], const float lsv2[3], const float lsdir2[3], float* min_d, float dir_d[2]);
__declspec_dll void mathPointProjectionPlane(const float p[3], const float plane_v[3], const float plane_n[3], float np[3], float* distance);

__declspec_dll float* mathPlaneNormalByVertices3(float vertices[3][3], float normal[3]);
__declspec_dll int mathPlaneHasPoint(const float plane_v[3], const float plane_normal[3], const float p[3]);
__declspec_dll int mathSegmentHasPoint(float ls[2][3], const float p[3]);
__declspec_dll int mathTriangleHasPoint(float tri[3][3], const float p[3], float* p_u, float* p_v);
__declspec_dll int mathSphereHasPoint(const float o[3], float radius, const float p[3]);
__declspec_dll int mathCapsuleHasPoint(const float o[3], const float axis[3], float radius, float half_height, const float p[3]);
__declspec_dll int mathAABBHasPoint(const float o[3], const float half[3], const float p[3]);

__declspec_dll float* mathTriangleGetPoint(float tri[3][3], float u, float v, float p[3]);

__declspec_dll int mathLineIntersectLine(const float ls1v[3], const float ls1dir[3], const float ls2v[3], const float ls2dir[3], float distance[2]);
__declspec_dll int mathLineIntersectPlane(const float ls_v[3], const float lsdir[3], const float plane_v[3], const float plane_normal[3], float* distance);
__declspec_dll int mathLineIntersectCapsule(const float ls_v[3], const float lsdir[3], const float o[3], const float axis[3], float radius, float half_height, float distance[2]);
__declspec_dll int mathSegmentIntersectSegment(float ls1[2][3], float ls2[2][3], float p[3]);
__declspec_dll int mathSegmentIntersectPlane(float ls[2][3], const float plane_v[3], const float plane_normal[3], float p[3]);
__declspec_dll int mathSegmentIntersectCapsule(float ls[2][3], const float o[3], const float axis[3], float radius, float half_height, float p[3]);
__declspec_dll int mathSphereIntersectLine(const float o[3], float radius, const float ls_vertice[3], const float lsdir[3], float distance[2]);
__declspec_dll int mathSphereIntersectSegment(const float o[3], float radius, float ls[2][3], float p[3]);
__declspec_dll int mathSphereIntersectPlane(const float o[3], float radius, const float plane_v[3], const float plane_normal[3], float new_o[3], float* new_r);
__declspec_dll int mathSphereIntersectTrianglesPlane(const float o[3], float radius, const float plane_n[3], float vertices[][3], const int indices[], int indicescnt);
__declspec_dll int mathSphereIntersectSphere(const float o1[3], float r1, const float o2[3], float r2, float p[3]);
__declspec_dll int mathSphereIntersectCapsule(const float sp_o[3], float sp_radius, const float cp_o[3], const float cp_axis[3], float cp_radius, float cp_half_height, float p[3]);
__declspec_dll int mathCapsuleIntersectPlane(const float cp_o[3], const float cp_axis[3], float cp_radius, float cp_half_height, const float plane_v[3], const float plane_n[3], float p[3]);
__declspec_dll int mathCapsuleIntersectTrianglesPlane(const float cp_o[3], const float cp_axis[3], float cp_radius, float cp_half_height, const float plane_n[3], float vertices[][3], const int indices[], int indicescnt);
__declspec_dll int mathCapsuleIntersectCapsule(const float cp1_o[3], const float cp1_axis[3], float cp1_radius, float cp1_half_height, const float cp2_o[3], const float cp2_axis[3], float cp2_radius, float cp2_half_height);
__declspec_dll int mathAABBIntersectAABB(const float o1[3], const float half1[3], const float o2[3], const float half2[3]);
__declspec_dll int mathAABBIntersectPlane(const float o[3], const float half[3], const float plane_v[3], const float plane_n[3]);
__declspec_dll int mathAABBIntersectSphere(const float aabb_o[3], const float aabb_half[3], const float sp_o[3], float radius);
__declspec_dll int mathAABBIntersectCapsule(const float aabb_o[3], const float aabb_half[3], const float cp_o[3], const float cp_axis[3], float cp_radius, float cp_half_height);
typedef struct CCTResult_t {
	float distance;
	int hit_point_cnt;
	float hit_point[3];
	float hit_normal[3];
} CCTResult_t;
__declspec_dll CCTResult_t* mathRaycastSegment(const float o[3], const float dir[3], float ls[2][3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathRaycastPlane(const float o[3], const float dir[3], const float plane_v[3], const float plane_n[3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathRaycastTriangle(const float o[3], const float dir[3], float tri[3][3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathRaycastSphere(const float o[3], const float dir[3], const float sp_o[3], float sp_radius, CCTResult_t* result);
__declspec_dll CCTResult_t* mathRaycastCapsule(const float o[3], const float dir[3], const float cp_o[3], const float cp_axis[3], float cp_radius, float cp_half_height, CCTResult_t* result);
__declspec_dll CCTResult_t* mathSegmentcastPlane(float ls[2][3], const float dir[3], const float vertice[3], const float normal[3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathSegmentcastSegment(float ls1[2][3], const float dir[3], float ls2[2][3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathSegmentcastTriangle(float ls[2][3], const float dir[3], float tri[3][3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathSegmentcastSphere(float ls[2][3], const float dir[3], const float center[3], float radius, CCTResult_t* result);
__declspec_dll CCTResult_t* mathSegmentcastCapsule(float ls[2][3], const float dir[3], const float cp_o[3], const float cp_axis[3], float cp_radius, float cp_half_height, CCTResult_t* result);
__declspec_dll CCTResult_t* mathTrianglecastPlane(float tri[3][3], const float dir[3], const float vertice[3], const float normal[3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathTrianglecastTriangle(float tri1[3][3], const float dir[3], float tri2[3][3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathAABBcastPlane(const float o[3], const float half[3], const float dir[3], const float vertice[3], const float normal[3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathAABBcastAABB(const float o1[3], const float half1[3], const float dir[3], const float o2[3], const float half2[3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathSpherecastPlane(const float o[3], float radius, const float dir[3], const float plane_v[3], const float plane_n[3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathSpherecastSphere(const float o1[3], float r1, const float dir[3], const float o2[3], float r2, CCTResult_t* result);
__declspec_dll CCTResult_t* mathSpherecastTrianglesPlane(const float o[3], float radius, const float dir[3], const float plane_n[3], float vertices[][3], const int indices[], int indicescnt, CCTResult_t* result);
__declspec_dll CCTResult_t* mathSpherecastAABB(const float o[3], float radius, const float dir[3], const float center[3], const float half[3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathSpherecastCapsule(const float sp_o[3], float sp_radius, const float dir[3], const float cp_o[3], const float cp_axis[3], float cp_radius, float cp_half_height, CCTResult_t* result);
__declspec_dll CCTResult_t* mathCapsulecastPlane(const float cp_o[3], const float cp_axis[3], float cp_radius, float cp_half_height, const float dir[3], const float plane_v[3], const float plane_n[3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathCapsulecastTrianglesPlane(const float cp_o[3], const float cp_axis[3], float cp_radius, float cp_half_height, const float dir[3], const float plane_n[3], float vertices[][3], const int indices[], int indicescnt, CCTResult_t* result);
__declspec_dll CCTResult_t* mathCapsulecastAABB(const float cp_o[3], const float cp_axis[3], float cp_radius, float cp_half_height, const float dir[3], const float aabb_o[3], const float aabb_half[3], CCTResult_t* result);
__declspec_dll CCTResult_t* mathCapsulecastCapsule(const float cp1_o[3], const float cp1_axis[3], float cp1_radius, float cp1_half_height, const float dir[3], const float cp2_o[3], const float cp2_axis[3], float cp2_radius, float cp2_half_height, CCTResult_t* result);

#ifdef	__cplusplus
}
#endif

#endif
