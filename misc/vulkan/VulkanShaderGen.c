/* Generates misc/vulkan/VulkanShaders.h */
/*   e.g. gcc VulkanShaderGen.c -o VulkanShaderGen && ./VulkanShaderGen > VulkanShaders.h */
/* SPIR-V is written out directly, so no shader compiler (glslang, shaderc etc) is needed */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html */
#define OP_EXTENSION             10
#define OP_EXT_INST_IMPORT       11
#define OP_EXT_INST              12
#define OP_MEMORY_MODEL          14
#define OP_ENTRY_POINT           15
#define OP_EXECUTION_MODE        16
#define OP_CAPABILITY            17
#define OP_TYPE_VOID             19
#define OP_TYPE_BOOL             20
#define OP_TYPE_INT              21
#define OP_TYPE_FLOAT            22
#define OP_TYPE_VECTOR           23
#define OP_TYPE_MATRIX           24
#define OP_TYPE_IMAGE            25
#define OP_TYPE_SAMPLED_IMAGE    27
#define OP_TYPE_STRUCT           30
#define OP_TYPE_POINTER          32
#define OP_TYPE_FUNCTION         33
#define OP_CONSTANT              43
#define OP_FUNCTION              54
#define OP_FUNCTION_END          56
#define OP_VARIABLE              59
#define OP_CONSTANT_COMPOSITE    44
#define OP_LOAD                  61
#define OP_STORE                 62
#define OP_ACCESS_CHAIN          65
#define OP_PTR_ACCESS_CHAIN      67
#define OP_DECORATE              71
#define OP_MEMBER_DECORATE       72
#define OP_VECTOR_SHUFFLE        79
#define OP_COMPOSITE_CONSTRUCT   80
#define OP_COMPOSITE_EXTRACT     81
#define OP_IMAGE_SAMPLE_IMPLICIT 87
#define OP_IMAGE_SAMPLE_EXPLICIT 88
#define OP_IADD                  128
#define OP_FADD                  129
#define OP_FSUB                  131
#define OP_IMUL                  132
#define OP_FMUL                  133
#define OP_FDIV                  136
#define OP_FNEGATE               127
#define OP_VECTOR_TIMES_SCALAR   142
#define OP_DOT                   148
#define OP_MATRIX_TIMES_VECTOR   145
#define OP_SELECT                169
#define OP_LOGICAL_AND           167
#define OP_LOGICAL_NOT           168
#define OP_INOT_EQUAL            171
#define OP_FORD_LESS_THAN        184
#define OP_FORD_GREATER_THAN     186
#define OP_FORD_GREATER_EQUAL    190
#define OP_SHIFT_RIGHT_LOGICAL   194
#define OP_SHIFT_LEFT_LOGICAL    196
#define OP_BITWISE_AND           199
#define OP_DPDX                  207
#define OP_DPDY                  208
#define OP_LOOP_MERGE            246
#define OP_SELECTION_MERGE       247
#define OP_LABEL                 248
#define OP_BRANCH                249
#define OP_BRANCH_CONDITIONAL    250
#define OP_KILL                  252
#define OP_RETURN                253

/* https://github.khronos.org/SPIRV-Registry/extensions/KHR/SPV_KHR_ray_query.html */
#define OP_TYPE_RAY_QUERY                  4472
#define OP_RAY_QUERY_INITIALIZE            4473
#define OP_RAY_QUERY_CONFIRM               4476
#define OP_RAY_QUERY_PROCEED               4477
#define OP_RAY_QUERY_GET_INTERSECTION_TYPE 4479
#define OP_RAY_QUERY_GET_CUSTOM_INDEX      6019
#define OP_RAY_QUERY_GET_GEOMETRY_INDEX    6022
#define OP_RAY_QUERY_GET_PRIMITIVE_INDEX   6023
#define OP_RAY_QUERY_GET_BARYCENTRICS      6024
#define OP_TYPE_ACCELERATION_STRUCTURE     5341

#define CAPABILITY_SHADER      1
#define CAPABILITY_RAY_QUERY   4472
#define CAPABILITY_PHYSICAL_STORAGE_BUFFER 5347
#define ADDRESSING_LOGICAL     0
#define ADDRESSING_PHYSICAL_STORAGE_BUFFER 5348
#define MEMORY_GLSL450         1
#define MODEL_VERTEX           0
#define MODEL_FRAGMENT         4
#define MODE_ORIGIN_UPPER_LEFT 7

#define STORAGE_UNIFORM_CONSTANT 0
#define STORAGE_INPUT            1
#define STORAGE_UNIFORM          2
#define STORAGE_OUTPUT           3
#define STORAGE_FUNCTION         7
#define STORAGE_PUSH_CONSTANT    9
#define STORAGE_PHYSICAL_BUFFER  5349

#define DECORATION_BLOCK          2
#define DECORATION_ARRAY_STRIDE   6
#define DECORATION_COL_MAJOR      5
#define DECORATION_MATRIX_STRIDE  7
#define DECORATION_BUILTIN        11
#define DECORATION_INVARIANT      18
#define DECORATION_LOCATION       30
#define DECORATION_BINDING        33
#define DECORATION_DESCRIPTOR_SET 34
#define DECORATION_OFFSET         35

#define BUILTIN_POSITION   0
#define BUILTIN_FRAG_COORD 15

/* https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html */
#define GLSL_FABS   4
#define GLSL_FSIGN  6
#define GLSL_EXP    27
#define GLSL_FMAX   40
#define GLSL_FCLAMP 43
#define GLSL_FMIX   46
#define GLSL_LENGTH    66
#define GLSL_CROSS     68
#define GLSL_NORMALIZE 69

#define FLOAT_ZERO 0x00000000u
#define FLOAT_HALF 0x3F000000u
#define FLOAT_ONE  0x3F800000u
#define FLOAT_RAY_OFFSET 0x3D4CCCCDu /* 0.05 */
#define FLOAT_FULLBRIGHT 0x3F7F7CEEu /* 0.998 */
#define FLOAT_AXIS_NORMAL 0x3F666666u /* 0.9 */
#define FLOAT_MIN_FACING  0x3C23D70Au /* 0.01 */

/* gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT */
/* Terminate on first hit (opaque flag isn't used, so that leaves etc can be alpha tested) */
#define RAY_FLAGS_SHADOW 4
#define RAY_INTERSECTION_CANDIDATE 0
#define RAY_INTERSECTION_COMMITTED 1
#define MEMORY_ALIGNED 2
#define IMAGE_OPERAND_LOD 2

#define SPIRV_1_0 0x00010000u
#define SPIRV_1_4 0x00010400u

#define FOG_NONE    0
#define FOG_LINEAR  1
#define FOG_DENSITY 2

static unsigned int code[8192];
static int len;
static unsigned int nextId, version;

static unsigned int Id(void) { return nextId++; }

static void Op(int opcode, int count, ...) {
	va_list args;
	int i;

	code[len++] = ((unsigned int)(count + 1) << 16) | (unsigned int)opcode;
	va_start(args, count);
	for (i = 0; i < count; i++) code[len++] = va_arg(args, unsigned int);
	va_end(args);
}

static int StringWords(const char* str) { return (int)strlen(str) / 4 + 1; }

static void EmitString(const char* str) {
	int i, j, count = StringWords(str), length = (int)strlen(str);
	unsigned int word;

	for (i = 0; i < count; i++)
	{
		word = 0;
		for (j = 0; j < 4; j++)
		{
			int k = i * 4 + j;
			if (k < length) word |= (unsigned int)(unsigned char)str[k] << (8 * j);
		}
		code[len++] = word;
	}
}

static void Extension(const char* name) {
	code[len++] = ((unsigned int)(1 + StringWords(name)) << 16) | OP_EXTENSION;
	EmitString(name);
}

static void ExtInstImport(unsigned int id) {
	static const char name[] = "GLSL.std.450";
	code[len++] = ((unsigned int)(2 + StringWords(name)) << 16) | OP_EXT_INST_IMPORT;
	code[len++] = id;
	EmitString(name);
}

static void EntryPoint(unsigned int model, unsigned int func, int count, const unsigned int* iface) {
	int i;
	code[len++] = ((unsigned int)(3 + StringWords("main") + count) << 16) | OP_ENTRY_POINT;
	code[len++] = model;
	code[len++] = func;
	EmitString("main");
	for (i = 0; i < count; i++) code[len++] = iface[i];
}

/* Words 0-4 are the header, which is filled in by End() */
static void Begin(unsigned int spirvVersion) {
	len     = 5;
	nextId  = 1;
	version = spirvVersion;
	Op(OP_CAPABILITY, 1, CAPABILITY_SHADER);
}

static void End(const char* name) {
	int i, j, count;
	code[0] = 0x07230203u;
	code[1] = version;
	code[2] = 0;
	code[3] = nextId;
	code[4] = 0;

	printf("static const unsigned int %s[%i] = {\n", name, len);
	for (i = 0; i < len; i += 8)
	{
		count = len - i < 8 ? len - i : 8;
		printf("\t");
		for (j = 0; j < count; j++) printf("0x%08x,", code[i + j]);
		printf("\n");
	}
	printf("};\n");
}

/* Equivalent GLSL:
layout(push_constant) uniform Constants { mat4 mvp; vec2 texOffset; };
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_col;
layout(location = 2) in vec2 in_uv;      (textured only)
layout(location = 0) out vec4 out_col;
layout(location = 1) out vec2 out_uv;    (textured only)
layout(location = 2) out vec3 out_world; (ray traced shadows only)

void main() {
	gl_Position = mvp * vec4(in_pos, 1.0);
	out_col     = in_col;
	out_uv      = in_uv + texOffset;
	out_world   = in_pos;
}
*/
static void VertexShader(const char* name, int textured, int worldPos) {
	unsigned int main_, tVoid, tFunc, tFloat, tVec2, tVec3, tVec4, tMat4, tInt, tPush;
	unsigned int pPush, pPushMat4, pPushVec2, pInVec2, pInVec3, pInVec4, pOutVec2, pOutVec4;
	unsigned int cOne, cInt0, cInt1;
	unsigned int vPos, vCol, vUV, vPosition, vOutCol, vOutUV, vPush;
	unsigned int pOutVec3 = 0, vOutWorld = 0;
	unsigned int label, pos, pos4, mvpPtr, mvp, position, col, uv, offsetPtr, offset, uvOffset;
	unsigned int iface[7];
	int count = 0;

	Begin(SPIRV_1_0);
	main_ = Id(); tVoid = Id(); tFunc = Id(); tFloat = Id(); tVec2 = Id(); tVec3 = Id(); tVec4 = Id(); tMat4 = Id(); tInt = Id(); tPush = Id();
	pPush = Id(); pPushMat4 = Id(); pPushVec2 = Id(); pInVec2 = Id(); pInVec3 = Id(); pInVec4 = Id(); pOutVec2 = Id(); pOutVec4 = Id();
	cOne  = Id(); cInt0 = Id(); cInt1 = Id();
	vPos  = Id(); vCol = Id(); vUV = Id(); vPosition = Id(); vOutCol = Id(); vOutUV = Id(); vPush = Id();
	label = Id(); pos = Id(); pos4 = Id(); mvpPtr = Id(); mvp = Id(); position = Id(); col = Id();
	uv    = Id(); offsetPtr = Id(); offset = Id(); uvOffset = Id();
	/* Only allocated when needed, so that the other shaders stay exactly the same */
	if (worldPos) { pOutVec3 = Id(); vOutWorld = Id(); }

	Op(OP_MEMORY_MODEL, 2, ADDRESSING_LOGICAL, MEMORY_GLSL450);
	iface[count++] = vPos;
	iface[count++] = vCol;
	if (textured) iface[count++] = vUV;
	iface[count++] = vPosition;
	iface[count++] = vOutCol;
	if (textured) iface[count++] = vOutUV;
	if (worldPos) iface[count++] = vOutWorld;
	EntryPoint(MODEL_VERTEX, main_, count, iface);

	Op(OP_DECORATE, 3, vPos, DECORATION_LOCATION, 0);
	Op(OP_DECORATE, 3, vCol, DECORATION_LOCATION, 1);
	if (textured) Op(OP_DECORATE, 3, vUV, DECORATION_LOCATION, 2);
	Op(OP_DECORATE, 3, vPosition, DECORATION_BUILTIN, BUILTIN_POSITION);
	/* Ensures depth only and colour passes produce exactly the same depth */
	Op(OP_DECORATE, 2, vPosition, DECORATION_INVARIANT);
	Op(OP_DECORATE, 3, vOutCol, DECORATION_LOCATION, 0);
	if (textured) Op(OP_DECORATE, 3, vOutUV, DECORATION_LOCATION, 1);
	if (worldPos) Op(OP_DECORATE, 3, vOutWorld, DECORATION_LOCATION, 2);
	Op(OP_DECORATE, 2, tPush, DECORATION_BLOCK);
	Op(OP_MEMBER_DECORATE, 3, tPush, 0, DECORATION_COL_MAJOR);
	Op(OP_MEMBER_DECORATE, 4, tPush, 0, DECORATION_OFFSET, 0);
	Op(OP_MEMBER_DECORATE, 4, tPush, 0, DECORATION_MATRIX_STRIDE, 16);
	Op(OP_MEMBER_DECORATE, 4, tPush, 1, DECORATION_OFFSET, 64);

	Op(OP_TYPE_VOID,     1, tVoid);
	Op(OP_TYPE_FUNCTION, 2, tFunc, tVoid);
	Op(OP_TYPE_FLOAT,    2, tFloat, 32);
	Op(OP_TYPE_VECTOR,   3, tVec2, tFloat, 2);
	Op(OP_TYPE_VECTOR,   3, tVec3, tFloat, 3);
	Op(OP_TYPE_VECTOR,   3, tVec4, tFloat, 4);
	Op(OP_TYPE_MATRIX,   3, tMat4, tVec4, 4);
	Op(OP_TYPE_INT,      3, tInt, 32, 1);
	Op(OP_TYPE_STRUCT,   3, tPush, tMat4, tVec2);
	Op(OP_TYPE_POINTER,  3, pPush,     STORAGE_PUSH_CONSTANT, tPush);
	Op(OP_TYPE_POINTER,  3, pPushMat4, STORAGE_PUSH_CONSTANT, tMat4);
	Op(OP_TYPE_POINTER,  3, pPushVec2, STORAGE_PUSH_CONSTANT, tVec2);
	Op(OP_TYPE_POINTER,  3, pInVec2,   STORAGE_INPUT,  tVec2);
	Op(OP_TYPE_POINTER,  3, pInVec3,   STORAGE_INPUT,  tVec3);
	Op(OP_TYPE_POINTER,  3, pInVec4,   STORAGE_INPUT,  tVec4);
	Op(OP_TYPE_POINTER,  3, pOutVec2,  STORAGE_OUTPUT, tVec2);
	Op(OP_TYPE_POINTER,  3, pOutVec4,  STORAGE_OUTPUT, tVec4);
	if (worldPos) Op(OP_TYPE_POINTER, 3, pOutVec3, STORAGE_OUTPUT, tVec3);
	Op(OP_CONSTANT,      3, tFloat, cOne,  FLOAT_ONE);
	Op(OP_CONSTANT,      3, tInt,   cInt0, 0);
	Op(OP_CONSTANT,      3, tInt,   cInt1, 1);

	Op(OP_VARIABLE, 3, pInVec3,  vPos, STORAGE_INPUT);
	Op(OP_VARIABLE, 3, pInVec4,  vCol, STORAGE_INPUT);
	if (textured) Op(OP_VARIABLE, 3, pInVec2, vUV, STORAGE_INPUT);
	Op(OP_VARIABLE, 3, pOutVec4, vPosition, STORAGE_OUTPUT);
	Op(OP_VARIABLE, 3, pOutVec4, vOutCol,   STORAGE_OUTPUT);
	if (textured) Op(OP_VARIABLE, 3, pOutVec2, vOutUV,    STORAGE_OUTPUT);
	if (worldPos) Op(OP_VARIABLE, 3, pOutVec3, vOutWorld, STORAGE_OUTPUT);
	Op(OP_VARIABLE, 3, pPush, vPush, STORAGE_PUSH_CONSTANT);

	Op(OP_FUNCTION, 4, tVoid, main_, 0, tFunc);
	Op(OP_LABEL,    1, label);
	Op(OP_LOAD,     3, tVec3, pos, vPos);
	Op(OP_COMPOSITE_CONSTRUCT, 4, tVec4, pos4, pos, cOne);
	Op(OP_ACCESS_CHAIN,        4, pPushMat4, mvpPtr, vPush, cInt0);
	Op(OP_LOAD,                3, tMat4, mvp, mvpPtr);
	Op(OP_MATRIX_TIMES_VECTOR, 4, tVec4, position, mvp, pos4);
	Op(OP_STORE, 2, vPosition, position);
	Op(OP_LOAD,  3, tVec4, col, vCol);
	Op(OP_STORE, 2, vOutCol, col);

	if (textured) {
		Op(OP_LOAD,         3, tVec2, uv, vUV);
		Op(OP_ACCESS_CHAIN, 4, pPushVec2, offsetPtr, vPush, cInt1);
		Op(OP_LOAD,         3, tVec2, offset, offsetPtr);
		Op(OP_FADD,         4, tVec2, uvOffset, uv, offset);
		Op(OP_STORE,        2, vOutUV, uvOffset);
	}
	/* Chunk vertices are already in world space */
	if (worldPos) Op(OP_STORE, 2, vOutWorld, pos);

	Op(OP_RETURN, 0);
	Op(OP_FUNCTION_END, 0);
	End(name);
}

/* Equivalent GLSL:
#extension GL_EXT_ray_query : require                (ray traced shadows only)
#extension GL_EXT_buffer_reference : require          (ray traced shadows only)
layout(set = 0, binding = 0) uniform sampler2D tex;   (textured only)
layout(buffer_reference) buffer UVs   { vec2 uv[]; };  (ray traced shadows only, 3 vec2s per vertex)
layout(buffer_reference) buffer Table { UVs vbs[]; };  (ray traced shadows only)
layout(set = 1, binding = 0) uniform accelerationStructureEXT scene; (ray traced shadows only)
layout(set = 1, binding = 1) uniform Params { Table table; };        (ray traced shadows only)
layout(push_constant) uniform Constants {
	layout(offset = 72) float camX, camY;               (ray traced shadows only)
	layout(offset = 80) vec3 fogCol; float fogEnd; float fogDensity;
	float shadowRatio; float camZ; vec3 sunDir; float shadowDist; (ray traced shadows only)
};
layout(location = 0) in vec4 in_col;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec3 in_world;               (ray traced shadows only)
layout(location = 0) out vec4 out_col;

void main() {
	vec4 col = texture(tex, in_uv) * in_col;
	if (col.a < 0.5) discard;                            (alpha test only)

	(ray traced shadows only)
	vec3 toCam   = vec3(camX, camY, camZ) - in_world;
	vec3 normal  = normalize(cross(dFdx(in_world), dFdy(in_world)));
	if (dot(normal, toCam) < 0.0) normal = -normal;
	vec3 a = abs(normal); float big = max(a.x, max(a.y, a.z));
	if (big > 0.9) normal = sign(normal * vec3(greaterThanEqual(a, vec3(big)))); (snap block faces to their axis)
	bool receive = length(toCam) < shadowDist && in_col.a > 0.998; (alpha 254 = fully bright)
	bool facing  = dot(normal, sunDir) > 0.01;
	float shade  = (receive && !facing) ? shadowRatio : 1.0;
	if (receive && facing) {
		rayQueryEXT rq;
		rayQueryInitializeEXT(rq, scene, gl_RayFlagsTerminateOnFirstHitEXT,
								0xFF, in_world + normal * 0.05, 0.0, sunDir, shadowDist);
		while (rayQueryProceedEXT(rq)) {
			UVs vb   = table.vbs[rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false) + rayQueryGetIntersectionGeometryIndexEXT(rq, false)];
			int prim = rayQueryGetIntersectionPrimitiveIndexEXT(rq, false);
			int base = (prim >> 1) << 2; bool odd = (prim & 1) != 0;
			vec2 uv0 = vb.uv[(base + (odd ? 2 : 0)) * 3 + 2];
			vec2 uv1 = vb.uv[(base + (odd ? 3 : 1)) * 3 + 2];
			vec2 uv2 = vb.uv[(base + (odd ? 0 : 2)) * 3 + 2];
			vec2 b   = rayQueryGetIntersectionBarycentricsEXT(rq, false);
			vec2 uv  = uv0 * (1.0 - b.x - b.y) + uv1 * b.x + uv2 * b.y;
			if (textureLod(tex, uv, 0.0).a >= 0.5) rayQueryConfirmIntersectionEXT(rq);
		}
		if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) shade = shadowRatio;
	}
	col.rgb *= shade;

	float depth = 1.0 / gl_FragCoord.w;
	float f = clamp(1.0 - depth * fogEnd, 0.0, 1.0);     (linear fog)
	float f = clamp(exp(fogDensity * depth), 0.0, 1.0);  (density fog)
	col.rgb = mix(fogCol, col.rgb, f);
	out_col = col;
}
*/
static void FragmentShader(const char* name, int textured, int alphaTest, int fog, int rt) {
	unsigned int ext, main_, tVoid, tFunc, tFloat, tVec2, tVec3, tVec4, tBool, tInt, tImage, tSampled, tPush;
	unsigned int pSampled, pPush, pPushVec3, pPushFloat, pInVec2, pInVec4, pOutVec4;
	unsigned int cZero, cOne, cHalf, cInt0, cInt1, cInt2;
	unsigned int vCol, vUV, vFragCoord, vOut, vTex, vPush;
	unsigned int label, col, sampler, uv, texCol, alpha, discard, killLabel, mergeLabel;
	unsigned int fragCoord, w, depth, paramPtr, param, scaled, value, fogFactor, fogPtr, fogCol, rgb, factors, mixed;
	unsigned int tUint = 0, tScene = 0, tRayQuery = 0, pScene = 0, pRayQuery = 0, pInVec3 = 0, pFuncFloat = 0;
	unsigned int cFlags = 0, cMask = 0, cCommitted = 0, cNone = 0, cOffset = 0, cBright = 0;
	unsigned int vWorld = 0, vScene = 0, rayQuery = 0, vShade = 0;
	unsigned int cIdx[9];
	unsigned int world, cam, camX, camY, camZ, toCam, dx, dy, crossed, normal, flipped, negated, flip, toCamDot;
	unsigned int sun, dist, ratio, scene, len, inRange, vertexCol, vertexAlpha, bright, receive, ndl, facing, notFacing;
	unsigned int backShadow, doRay, shadeDefault, rayLabel, rayMerge, offset, origin, proceed, hitType, hit, shadeHit, shade, shaded;
	unsigned int tBVec3 = 0, tUVPtr = 0, tTablePtr = 0, tParams = 0, pParams = 0, pParamsTable = 0, vParams = 0;
	unsigned int cVec3Zero = 0, cVec3One = 0, cAxis = 0, cMinFacing = 0;
	unsigned int iface[10];
	int i, count = 0, push = fog || rt, fogBase = rt ? 2 : 0;

	Begin(rt ? SPIRV_1_4 : SPIRV_1_0);
	ext   = Id(); main_ = Id(); tVoid = Id(); tFunc = Id(); tFloat = Id(); tVec2 = Id(); tVec3 = Id(); tVec4 = Id(); tBool = Id(); tInt = Id();
	tImage = Id(); tSampled = Id(); tPush = Id();
	pSampled = Id(); pPush = Id(); pPushVec3 = Id(); pPushFloat = Id(); pInVec2 = Id(); pInVec4 = Id(); pOutVec4 = Id();
	cZero = Id(); cOne = Id(); cHalf = Id(); cInt0 = Id(); cInt1 = Id(); cInt2 = Id();
	vCol  = Id(); vUV = Id(); vFragCoord = Id(); vOut = Id(); vTex = Id(); vPush = Id();
	label = Id(); col = Id();

	cIdx[0] = cInt0; cIdx[1] = cInt1; cIdx[2] = cInt2;
	/* Only allocated when needed, so that the other shaders stay exactly the same */
	if (rt) {
		tUint = Id(); tScene = Id(); tRayQuery = Id(); pScene = Id(); pRayQuery = Id(); pInVec3 = Id(); pFuncFloat = Id();
		cFlags = Id(); cMask = Id(); cCommitted = Id(); cNone = Id(); cOffset = Id(); cBright = Id();
		vWorld = Id(); vScene = Id(); rayQuery = Id(); vShade = Id();
		for (i = 3; i < 9; i++) cIdx[i] = Id();
		tBVec3 = Id(); tUVPtr = Id(); tTablePtr = Id(); tParams = Id(); pParams = Id(); pParamsTable = Id(); vParams = Id();
		cVec3Zero = Id(); cVec3One = Id(); cAxis = Id(); cMinFacing = Id();
	}

	if (rt) {
		Op(OP_CAPABILITY, 1, CAPABILITY_RAY_QUERY);
		Op(OP_CAPABILITY, 1, CAPABILITY_PHYSICAL_STORAGE_BUFFER);
		Extension("SPV_KHR_ray_query");
		Extension("SPV_KHR_physical_storage_buffer");
	}
	if (fog || rt) ExtInstImport(ext);
	Op(OP_MEMORY_MODEL, 2, rt ? ADDRESSING_PHYSICAL_STORAGE_BUFFER : ADDRESSING_LOGICAL, MEMORY_GLSL450);

	/* SPIR-V 1.4 requires all global variables used to be listed, not just inputs/outputs */
	iface[count++] = vCol;
	if (textured) iface[count++] = vUV;
	if (fog)      iface[count++] = vFragCoord;
	iface[count++] = vOut;
	if (rt) {
		iface[count++] = vWorld;
		iface[count++] = vScene;
		iface[count++] = vPush;
		iface[count++] = vParams;
		if (textured) iface[count++] = vTex;
	}
	EntryPoint(MODEL_FRAGMENT, main_, count, iface);
	Op(OP_EXECUTION_MODE, 2, main_, MODE_ORIGIN_UPPER_LEFT);

	Op(OP_DECORATE, 3, vCol, DECORATION_LOCATION, 0);
	if (textured) Op(OP_DECORATE, 3, vUV, DECORATION_LOCATION, 1);
	if (fog)      Op(OP_DECORATE, 3, vFragCoord, DECORATION_BUILTIN, BUILTIN_FRAG_COORD);
	Op(OP_DECORATE, 3, vOut, DECORATION_LOCATION, 0);
	if (textured) {
		Op(OP_DECORATE, 3, vTex, DECORATION_DESCRIPTOR_SET, 0);
		Op(OP_DECORATE, 3, vTex, DECORATION_BINDING, 0);
	}
	if (rt) {
		Op(OP_DECORATE, 2, tPush, DECORATION_BLOCK);
		Op(OP_MEMBER_DECORATE, 4, tPush, 0, DECORATION_OFFSET, 72);
		Op(OP_MEMBER_DECORATE, 4, tPush, 1, DECORATION_OFFSET, 76);
		Op(OP_MEMBER_DECORATE, 4, tPush, 2, DECORATION_OFFSET, 80);
		Op(OP_MEMBER_DECORATE, 4, tPush, 3, DECORATION_OFFSET, 92);
		Op(OP_MEMBER_DECORATE, 4, tPush, 4, DECORATION_OFFSET, 96);
		Op(OP_MEMBER_DECORATE, 4, tPush, 5, DECORATION_OFFSET, 100);
		Op(OP_MEMBER_DECORATE, 4, tPush, 6, DECORATION_OFFSET, 104);
		Op(OP_MEMBER_DECORATE, 4, tPush, 7, DECORATION_OFFSET, 112);
		Op(OP_MEMBER_DECORATE, 4, tPush, 8, DECORATION_OFFSET, 124);
		Op(OP_DECORATE, 3, vWorld, DECORATION_LOCATION, 2);
		Op(OP_DECORATE, 3, vScene, DECORATION_DESCRIPTOR_SET, 1);
		Op(OP_DECORATE, 3, vScene, DECORATION_BINDING, 0);
		/* Vertices are 24 bytes, so are read as 3 vec2s, with the UV being the 3rd */
		Op(OP_DECORATE, 3, tUVPtr,    DECORATION_ARRAY_STRIDE, 8);
		Op(OP_DECORATE, 3, tTablePtr, DECORATION_ARRAY_STRIDE, 8);
		Op(OP_DECORATE, 2, tParams, DECORATION_BLOCK);
		Op(OP_MEMBER_DECORATE, 4, tParams, 0, DECORATION_OFFSET, 0);
		Op(OP_DECORATE, 3, vParams, DECORATION_DESCRIPTOR_SET, 1);
		Op(OP_DECORATE, 3, vParams, DECORATION_BINDING, 1);
	} else if (fog) {
		Op(OP_DECORATE, 2, tPush, DECORATION_BLOCK);
		Op(OP_MEMBER_DECORATE, 4, tPush, 0, DECORATION_OFFSET, 80);
		Op(OP_MEMBER_DECORATE, 4, tPush, 1, DECORATION_OFFSET, 92);
		Op(OP_MEMBER_DECORATE, 4, tPush, 2, DECORATION_OFFSET, 96);
	}

	Op(OP_TYPE_VOID,     1, tVoid);
	Op(OP_TYPE_FUNCTION, 2, tFunc, tVoid);
	Op(OP_TYPE_FLOAT,    2, tFloat, 32);
	Op(OP_TYPE_VECTOR,   3, tVec2, tFloat, 2);
	Op(OP_TYPE_VECTOR,   3, tVec3, tFloat, 3);
	Op(OP_TYPE_VECTOR,   3, tVec4, tFloat, 4);
	Op(OP_TYPE_BOOL,     1, tBool);
	Op(OP_TYPE_INT,      3, tInt, 32, 1);
	if (textured) {
		Op(OP_TYPE_IMAGE,         8, tImage, tFloat, 1, 0, 0, 0, 1, 0);
		Op(OP_TYPE_SAMPLED_IMAGE, 2, tSampled, tImage);
		Op(OP_TYPE_POINTER,       3, pSampled, STORAGE_UNIFORM_CONSTANT, tSampled);
	}
	if (rt) {
		Op(OP_TYPE_STRUCT, 10, tPush, tFloat, tFloat, tVec3, tFloat, tFloat, tFloat, tFloat, tVec3, tFloat);
	} else if (fog) {
		Op(OP_TYPE_STRUCT,  4, tPush, tVec3, tFloat, tFloat);
	}
	if (push) {
		Op(OP_TYPE_POINTER, 3, pPush,      STORAGE_PUSH_CONSTANT, tPush);
		Op(OP_TYPE_POINTER, 3, pPushVec3,  STORAGE_PUSH_CONSTANT, tVec3);
		Op(OP_TYPE_POINTER, 3, pPushFloat, STORAGE_PUSH_CONSTANT, tFloat);
	}
	Op(OP_TYPE_POINTER, 3, pInVec2,  STORAGE_INPUT,  tVec2);
	Op(OP_TYPE_POINTER, 3, pInVec4,  STORAGE_INPUT,  tVec4);
	Op(OP_TYPE_POINTER, 3, pOutVec4, STORAGE_OUTPUT, tVec4);
	if (rt) {
		Op(OP_TYPE_INT,     3, tUint, 32, 0);
		Op(OP_TYPE_ACCELERATION_STRUCTURE, 1, tScene);
		Op(OP_TYPE_RAY_QUERY,              1, tRayQuery);
		Op(OP_TYPE_POINTER, 3, pScene,     STORAGE_UNIFORM_CONSTANT, tScene);
		Op(OP_TYPE_POINTER, 3, pRayQuery,  STORAGE_FUNCTION, tRayQuery);
		Op(OP_TYPE_POINTER, 3, pInVec3,    STORAGE_INPUT,    tVec3);
		Op(OP_TYPE_POINTER, 3, pFuncFloat, STORAGE_FUNCTION, tFloat);
		Op(OP_TYPE_VECTOR,  3, tBVec3, tBool, 3);
		Op(OP_TYPE_POINTER, 3, tUVPtr,    STORAGE_PHYSICAL_BUFFER, tVec2);
		Op(OP_TYPE_POINTER, 3, tTablePtr, STORAGE_PHYSICAL_BUFFER, tUVPtr);
		Op(OP_TYPE_STRUCT,  2, tParams, tTablePtr);
		Op(OP_TYPE_POINTER, 3, pParams,      STORAGE_UNIFORM, tParams);
		Op(OP_TYPE_POINTER, 3, pParamsTable, STORAGE_UNIFORM, tTablePtr);
	}
	Op(OP_CONSTANT, 3, tFloat, cZero, FLOAT_ZERO);
	Op(OP_CONSTANT, 3, tFloat, cOne,  FLOAT_ONE);
	Op(OP_CONSTANT, 3, tFloat, cHalf, FLOAT_HALF);
	Op(OP_CONSTANT, 3, tInt,   cInt0, 0);
	Op(OP_CONSTANT, 3, tInt,   cInt1, 1);
	Op(OP_CONSTANT, 3, tInt,   cInt2, 2);
	if (rt) {
		for (i = 3; i < 9; i++) Op(OP_CONSTANT, 3, tInt, cIdx[i], i);
		Op(OP_CONSTANT, 3, tUint,  cFlags,     RAY_FLAGS_SHADOW);
		Op(OP_CONSTANT, 3, tUint,  cMask,      0xFF);
		Op(OP_CONSTANT, 3, tUint,  cCommitted, RAY_INTERSECTION_COMMITTED);
		Op(OP_CONSTANT, 3, tUint,  cNone,      0);
		Op(OP_CONSTANT, 3, tFloat, cOffset,    FLOAT_RAY_OFFSET);
		Op(OP_CONSTANT, 3, tFloat, cBright,    FLOAT_FULLBRIGHT);
		Op(OP_CONSTANT, 3, tFloat, cAxis,      FLOAT_AXIS_NORMAL);
		Op(OP_CONSTANT, 3, tFloat, cMinFacing, FLOAT_MIN_FACING);
		Op(OP_CONSTANT_COMPOSITE, 5, tVec3, cVec3Zero, cZero, cZero, cZero);
		Op(OP_CONSTANT_COMPOSITE, 5, tVec3, cVec3One,  cOne,  cOne,  cOne);
	}

	Op(OP_VARIABLE, 3, pInVec4, vCol, STORAGE_INPUT);
	if (textured) Op(OP_VARIABLE, 3, pInVec2, vUV, STORAGE_INPUT);
	if (fog)      Op(OP_VARIABLE, 3, pInVec4, vFragCoord, STORAGE_INPUT);
	Op(OP_VARIABLE, 3, pOutVec4, vOut, STORAGE_OUTPUT);
	if (textured) Op(OP_VARIABLE, 3, pSampled, vTex, STORAGE_UNIFORM_CONSTANT);
	if (push)     Op(OP_VARIABLE, 3, pPush, vPush, STORAGE_PUSH_CONSTANT);
	if (rt) {
		Op(OP_VARIABLE, 3, pInVec3, vWorld, STORAGE_INPUT);
		Op(OP_VARIABLE, 3, pScene,  vScene, STORAGE_UNIFORM_CONSTANT);
		Op(OP_VARIABLE, 3, pParams, vParams, STORAGE_UNIFORM);
	}

	Op(OP_FUNCTION, 4, tVoid, main_, 0, tFunc);
	Op(OP_LABEL,    1, label);
	/* Function variables must be declared at the very start of the first block */
	if (rt) {
		Op(OP_VARIABLE, 3, pRayQuery,  rayQuery, STORAGE_FUNCTION);
		Op(OP_VARIABLE, 3, pFuncFloat, vShade,   STORAGE_FUNCTION);
	}
	Op(OP_LOAD,     3, tVec4, col, vCol);
	vertexCol = col;

	if (textured) {
		sampler = Id(); uv = Id(); texCol = Id();
		Op(OP_LOAD, 3, tSampled, sampler, vTex);
		Op(OP_LOAD, 3, tVec2, uv, vUV);
		Op(OP_IMAGE_SAMPLE_IMPLICIT, 4, tVec4, texCol, sampler, uv);
		value = Id();
		Op(OP_FMUL, 4, tVec4, value, texCol, col);
		col = value;
	}

	if (alphaTest) {
		alpha = Id(); discard = Id(); killLabel = Id(); mergeLabel = Id();
		Op(OP_COMPOSITE_EXTRACT,  4, tFloat, alpha, col, 3);
		Op(OP_FORD_LESS_THAN,     4, tBool, discard, alpha, cHalf);
		Op(OP_SELECTION_MERGE,    2, mergeLabel, 0);
		Op(OP_BRANCH_CONDITIONAL, 3, discard, killLabel, mergeLabel);
		Op(OP_LABEL, 1, killLabel);
		Op(OP_KILL,  0);
		Op(OP_LABEL, 1, mergeLabel);
	}

	if (rt) {
		world = Id(); camX = Id(); camY = Id(); camZ = Id(); cam = Id(); toCam = Id(); dx = Id(); dy = Id();
		crossed = Id(); normal = Id(); toCamDot = Id(); flip = Id(); negated = Id(); flipped = Id();
		sun = Id(); dist = Id(); ratio = Id(); scene = Id(); len = Id(); inRange = Id();
		vertexAlpha = Id(); bright = Id(); receive = Id(); ndl = Id(); facing = Id(); notFacing = Id();
		backShadow = Id(); doRay = Id(); shadeDefault = Id(); rayLabel = Id(); rayMerge = Id();
		offset = Id(); origin = Id(); proceed = Id(); hitType = Id(); hit = Id(); shadeHit = Id(); shade = Id();

		Op(OP_LOAD, 3, tVec3, world, vWorld);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pPushFloat, paramPtr, vPush, cIdx[0]); Op(OP_LOAD, 3, tFloat, camX, paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pPushFloat, paramPtr, vPush, cIdx[1]); Op(OP_LOAD, 3, tFloat, camY, paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pPushFloat, paramPtr, vPush, cIdx[6]); Op(OP_LOAD, 3, tFloat, camZ, paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pPushVec3,  paramPtr, vPush, cIdx[7]); Op(OP_LOAD, 3, tVec3,  sun,  paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pPushFloat, paramPtr, vPush, cIdx[8]); Op(OP_LOAD, 3, tFloat, dist, paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pPushFloat, paramPtr, vPush, cIdx[5]); Op(OP_LOAD, 3, tFloat, ratio, paramPtr);
		Op(OP_COMPOSITE_CONSTRUCT, 5, tVec3, cam, camX, camY, camZ);
		Op(OP_FSUB, 4, tVec3, toCam, cam, world);

		/* Faces are flat, so the normal can be calculated from the change in position across pixels */
		Op(OP_DPDX, 3, tVec3, dx, world);
		Op(OP_DPDY, 3, tVec3, dy, world);
		Op(OP_EXT_INST, 6, tVec3, crossed, ext, GLSL_CROSS, dx, dy);
		Op(OP_EXT_INST, 5, tVec3, normal,  ext, GLSL_NORMALIZE, crossed);
		Op(OP_DOT,      4, tFloat, toCamDot, normal, toCam);
		Op(OP_FORD_LESS_THAN, 4, tBool, flip, toCamDot, cZero);
		Op(OP_FNEGATE,  3, tVec3, negated, normal);
		Op(OP_SELECT,   5, tVec3, flipped, flip, negated, normal);

		/* The calculated normal is slightly inaccurate, which causes noise on faces that are side on to the sun */
		/*  (since some pixels end up facing towards the sun, and others away). Block faces are axis aligned though, */
		/*  so their normal can be snapped to the closest axis (sprites aren't, so they keep the calculated normal) */
		{
			unsigned int absN = Id(), ax = Id(), ay = Id(), az = Id(), bigYZ = Id(), big = Id(), bigs = Id();
			unsigned int isBig = Id(), mask = Id(), masked = Id(), snapped = Id(), axisAligned = Id(), axisNormal = Id();

			Op(OP_EXT_INST, 5, tVec3, absN, ext, GLSL_FABS, flipped);
			Op(OP_COMPOSITE_EXTRACT, 4, tFloat, ax, absN, 0);
			Op(OP_COMPOSITE_EXTRACT, 4, tFloat, ay, absN, 1);
			Op(OP_COMPOSITE_EXTRACT, 4, tFloat, az, absN, 2);
			Op(OP_EXT_INST, 6, tFloat, bigYZ, ext, GLSL_FMAX, ay, az);
			Op(OP_EXT_INST, 6, tFloat, big,   ext, GLSL_FMAX, ax, bigYZ);
			Op(OP_COMPOSITE_CONSTRUCT, 5, tVec3, bigs, big, big, big);
			Op(OP_FORD_GREATER_EQUAL,  4, tBVec3, isBig, absN, bigs);
			Op(OP_SELECT,   5, tVec3, mask, isBig, cVec3One, cVec3Zero);
			Op(OP_FMUL,     4, tVec3, masked, flipped, mask);
			Op(OP_EXT_INST, 5, tVec3, snapped, ext, GLSL_FSIGN, masked);
			Op(OP_FORD_GREATER_THAN, 4, tBool, axisAligned, big, cAxis);
			Op(OP_SELECT,   5, tVec3, axisNormal, axisAligned, snapped, flipped);
			flipped = axisNormal;
		}

		/* Fully bright faces (alpha 254), and faces beyond the shadow distance aren't shadowed */
		Op(OP_EXT_INST, 5, tFloat, len, ext, GLSL_LENGTH, toCam);
		Op(OP_FORD_LESS_THAN, 4, tBool, inRange, len, dist);
		Op(OP_COMPOSITE_EXTRACT, 4, tFloat, vertexAlpha, vertexCol, 3);
		Op(OP_FORD_GREATER_THAN, 4, tBool, bright, vertexAlpha, cBright);
		Op(OP_LOGICAL_AND, 4, tBool, receive, inRange, bright);

		/* Faces pointing away from the sun are always in shadow, so don't need a ray */
		Op(OP_DOT, 4, tFloat, ndl, flipped, sun);
		Op(OP_FORD_GREATER_THAN, 4, tBool, facing, ndl, cMinFacing);
		Op(OP_LOGICAL_NOT, 3, tBool, notFacing, facing);
		Op(OP_LOGICAL_AND, 4, tBool, backShadow, receive, notFacing);
		Op(OP_LOGICAL_AND, 4, tBool, doRay,      receive, facing);
		Op(OP_SELECT, 5, tFloat, shadeDefault, backShadow, ratio, cOne);
		Op(OP_STORE,  2, vShade, shadeDefault);

		Op(OP_SELECTION_MERGE,    2, rayMerge, 0);
		Op(OP_BRANCH_CONDITIONAL, 3, doRay, rayLabel, rayMerge);
		Op(OP_LABEL, 1, rayLabel);
		{
			/* Start the ray slightly off the surface, to avoid the ray hitting the face it started on */
			Op(OP_VECTOR_TIMES_SCALAR, 4, tVec3, offset, flipped, cOffset);
			Op(OP_FADD, 4, tVec3, origin, world, offset);
			Op(OP_LOAD, 3, tScene, scene, vScene);

			Op(OP_RAY_QUERY_INITIALIZE, 8, rayQuery, scene, cFlags, cMask, origin, cZero, sun, dist);

			/* Opaque faces are hit automatically, but faces of blocks like leaves are only */
			/*  hit when the texture at the point the ray passes through isn't transparent */
			{
				unsigned int loopHeader = Id(), loopCond = Id(), loopBody = Id(), loopContinue = Id(), loopMerge = Id();
				unsigned int confirmLabel = Id(), bodyMerge = Id();
				unsigned int custom = Id(), geom = Id(), prim = Id(), bary = Id(), entry = Id(), tablePtr = Id(), table = Id();
				unsigned int entryPtr = Id(), uvs = Id(), quad = Id(), first = Id(), oddBit = Id(), odd = Id();
				unsigned int bx = Id(), by = Id(), w0a = Id(), w0 = Id(), t0 = Id(), t1 = Id(), t2 = Id(), t01 = Id(), uvHit = Id();
				unsigned int sampler2 = Id(), texel = Id(), texAlpha = Id(), solid = Id();
				/* Default index buffer uses vertices 0,1,2 for the first triangle of a quad, then 2,3,0 */
				static const int evenVertex[3] = { 0, 1, 2 }, oddVertex[3] = { 2, 3, 0 };
				unsigned int uvK[3];
				int k;

				Op(OP_BRANCH, 1, loopHeader);
				Op(OP_LABEL,  1, loopHeader);
				Op(OP_LOOP_MERGE, 3, loopMerge, loopContinue, 0);
				Op(OP_BRANCH, 1, loopCond);
				Op(OP_LABEL,  1, loopCond);
				Op(OP_RAY_QUERY_PROCEED,  3, tBool, proceed, rayQuery);
				Op(OP_BRANCH_CONDITIONAL, 3, proceed, loopBody, loopMerge);
				Op(OP_LABEL,  1, loopBody);

				/* Each instance's custom index is where its geometries start in the table of vertex buffers */
				Op(OP_RAY_QUERY_GET_CUSTOM_INDEX,    4, tInt,  custom, rayQuery, cNone);
				Op(OP_RAY_QUERY_GET_GEOMETRY_INDEX,  4, tInt,  geom,   rayQuery, cNone);
				Op(OP_RAY_QUERY_GET_PRIMITIVE_INDEX, 4, tInt,  prim,   rayQuery, cNone);
				Op(OP_RAY_QUERY_GET_BARYCENTRICS,    4, tVec2, bary,   rayQuery, cNone);
				Op(OP_IADD, 4, tInt, entry, custom, geom);
				Op(OP_ACCESS_CHAIN, 4, pParamsTable, tablePtr, vParams, cInt0);
				Op(OP_LOAD, 3, tTablePtr, table, tablePtr);
				Op(OP_PTR_ACCESS_CHAIN, 4, tTablePtr, entryPtr, table, entry);
				Op(OP_LOAD, 5, tUVPtr, uvs, entryPtr, MEMORY_ALIGNED, 8);

				Op(OP_SHIFT_RIGHT_LOGICAL, 4, tInt, quad,   prim, cInt1);
				Op(OP_SHIFT_LEFT_LOGICAL,  4, tInt, first,  quad, cInt2);
				Op(OP_BITWISE_AND,         4, tInt, oddBit, prim, cInt1);
				Op(OP_INOT_EQUAL,          4, tBool, odd, oddBit, cInt0);

				for (k = 0; k < 3; k++)
				{
					unsigned int sel = Id(), vertex = Id(), scaled3 = Id(), index = Id(), uvPtr = Id();
					uvK[k] = Id();
					Op(OP_SELECT, 5, tInt, sel, odd, cIdx[oddVertex[k]], cIdx[evenVertex[k]]);
					Op(OP_IADD,   4, tInt, vertex,  first, sel);
					Op(OP_IMUL,   4, tInt, scaled3, vertex, cIdx[3]);
					Op(OP_IADD,   4, tInt, index,   scaled3, cInt2);
					Op(OP_PTR_ACCESS_CHAIN, 4, tUVPtr, uvPtr, uvs, index);
					Op(OP_LOAD, 5, tVec2, uvK[k], uvPtr, MEMORY_ALIGNED, 8);
				}

				Op(OP_COMPOSITE_EXTRACT, 4, tFloat, bx, bary, 0);
				Op(OP_COMPOSITE_EXTRACT, 4, tFloat, by, bary, 1);
				Op(OP_FSUB, 4, tFloat, w0a, cOne, bx);
				Op(OP_FSUB, 4, tFloat, w0,  w0a,  by);
				Op(OP_VECTOR_TIMES_SCALAR, 4, tVec2, t0, uvK[0], w0);
				Op(OP_VECTOR_TIMES_SCALAR, 4, tVec2, t1, uvK[1], bx);
				Op(OP_VECTOR_TIMES_SCALAR, 4, tVec2, t2, uvK[2], by);
				Op(OP_FADD, 4, tVec2, t01,   t0,  t1);
				Op(OP_FADD, 4, tVec2, uvHit, t01, t2);

				Op(OP_LOAD, 3, tSampled, sampler2, vTex);
				Op(OP_IMAGE_SAMPLE_EXPLICIT, 6, tVec4, texel, sampler2, uvHit, IMAGE_OPERAND_LOD, cZero);
				Op(OP_COMPOSITE_EXTRACT, 4, tFloat, texAlpha, texel, 3);
				Op(OP_FORD_GREATER_EQUAL, 4, tBool, solid, texAlpha, cHalf);

				Op(OP_SELECTION_MERGE,    2, bodyMerge, 0);
				Op(OP_BRANCH_CONDITIONAL, 3, solid, confirmLabel, bodyMerge);
				Op(OP_LABEL, 1, confirmLabel);
				Op(OP_RAY_QUERY_CONFIRM, 1, rayQuery);
				Op(OP_BRANCH, 1, bodyMerge);
				Op(OP_LABEL,  1, bodyMerge);
				Op(OP_BRANCH, 1, loopContinue);
				Op(OP_LABEL,  1, loopContinue);
				Op(OP_BRANCH, 1, loopHeader);
				Op(OP_LABEL,  1, loopMerge);
			}
			Op(OP_RAY_QUERY_GET_INTERSECTION_TYPE, 4, tUint, hitType, rayQuery, cCommitted);
			Op(OP_INOT_EQUAL, 4, tBool,  hit,      hitType, cNone);
			Op(OP_SELECT,     5, tFloat, shadeHit, hit, ratio, cOne);
			Op(OP_STORE,  2, vShade, shadeHit);
			Op(OP_BRANCH, 1, rayMerge);
		}
		Op(OP_LABEL, 1, rayMerge);
		Op(OP_LOAD,  3, tFloat, shade, vShade);

		rgb = Id(); shaded = Id(); value = Id();
		Op(OP_VECTOR_SHUFFLE,      7, tVec3, rgb,    col, col, 0, 1, 2);
		Op(OP_VECTOR_TIMES_SCALAR, 4, tVec3, shaded, rgb, shade);
		Op(OP_VECTOR_SHUFFLE,      8, tVec4, value,  shaded, col, 0, 1, 2, 6);
		col = value;
	}

	if (fog) {
		fragCoord = Id(); w = Id(); depth = Id(); paramPtr = Id(); param = Id(); scaled = Id(); value = Id(); fogFactor = Id();
		fogPtr = Id(); fogCol = Id(); rgb = Id(); factors = Id(); mixed = Id();

		Op(OP_LOAD, 3, tVec4, fragCoord, vFragCoord);
		Op(OP_COMPOSITE_EXTRACT, 4, tFloat, w, fragCoord, 3);
		Op(OP_FDIV, 4, tFloat, depth, cOne, w);

		if (fog == FOG_LINEAR) {
			Op(OP_ACCESS_CHAIN, 4, pPushFloat, paramPtr, vPush, cIdx[fogBase + 1]);
			Op(OP_LOAD, 3, tFloat, param, paramPtr);
			Op(OP_FMUL, 4, tFloat, scaled, depth, param);
			Op(OP_FSUB, 4, tFloat, value, cOne, scaled);
		} else {
			Op(OP_ACCESS_CHAIN, 4, pPushFloat, paramPtr, vPush, cIdx[fogBase + 2]);
			Op(OP_LOAD, 3, tFloat, param, paramPtr);
			Op(OP_FMUL, 4, tFloat, scaled, param, depth);
			Op(OP_EXT_INST, 5, tFloat, value, ext, GLSL_EXP, scaled);
		}
		Op(OP_EXT_INST, 7, tFloat, fogFactor, ext, GLSL_FCLAMP, value, cZero, cOne);

		Op(OP_ACCESS_CHAIN, 4, pPushVec3, fogPtr, vPush, cIdx[fogBase]);
		Op(OP_LOAD, 3, tVec3, fogCol, fogPtr);
		Op(OP_VECTOR_SHUFFLE, 7, tVec3, rgb, col, col, 0, 1, 2);
		Op(OP_COMPOSITE_CONSTRUCT, 5, tVec3, factors, fogFactor, fogFactor, fogFactor);
		Op(OP_EXT_INST, 7, tVec3, mixed, ext, GLSL_FMIX, fogCol, rgb, factors);

		value = Id();
		Op(OP_VECTOR_SHUFFLE, 8, tVec4, value, mixed, col, 0, 1, 2, 6);
		col = value;
	}

	Op(OP_STORE, 2, vOut, col);
	Op(OP_RETURN, 0);
	Op(OP_FUNCTION_END, 0);
	End(name);
}

int main(void) {
	printf("// Generated using misc/vulkan/VulkanShaderGen.c\n\n");
	VertexShader("vs_colored",    0, 0);
	VertexShader("vs_textured",   1, 0);
	VertexShader("vs_textured_rt", 1, 1);
	printf("\n");

	FragmentShader("fs_colored",       0, 0, FOG_NONE, 0);
	FragmentShader("fs_textured",      1, 0, FOG_NONE, 0);
	FragmentShader("fs_colored_test",  0, 1, FOG_NONE, 0);
	FragmentShader("fs_textured_test", 1, 1, FOG_NONE, 0);

	FragmentShader("fs_colored_linear",       0, 0, FOG_LINEAR, 0);
	FragmentShader("fs_textured_linear",      1, 0, FOG_LINEAR, 0);
	FragmentShader("fs_colored_test_linear",  0, 1, FOG_LINEAR, 0);
	FragmentShader("fs_textured_test_linear", 1, 1, FOG_LINEAR, 0);

	FragmentShader("fs_colored_density",       0, 0, FOG_DENSITY, 0);
	FragmentShader("fs_textured_density",      1, 0, FOG_DENSITY, 0);
	FragmentShader("fs_colored_test_density",  0, 1, FOG_DENSITY, 0);
	FragmentShader("fs_textured_test_density", 1, 1, FOG_DENSITY, 0);
	printf("\n");

	FragmentShader("fs_textured_rt",              1, 0, FOG_NONE,    1);
	FragmentShader("fs_textured_test_rt",         1, 1, FOG_NONE,    1);
	FragmentShader("fs_textured_linear_rt",       1, 0, FOG_LINEAR,  1);
	FragmentShader("fs_textured_test_linear_rt",  1, 1, FOG_LINEAR,  1);
	FragmentShader("fs_textured_density_rt",      1, 0, FOG_DENSITY, 1);
	FragmentShader("fs_textured_test_density_rt", 1, 1, FOG_DENSITY, 1);
	return 0;
}
