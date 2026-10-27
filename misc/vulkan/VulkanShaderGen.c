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
#define OP_IMAGE_FETCH           95
#define OP_IMAGE                 100
#define OP_CONVERT_F_TO_S        110
#define OP_CONVERT_S_TO_F        111
#define OP_IADD                  128
#define OP_FADD                  129
#define OP_ISUB                  130
#define OP_FSUB                  131
#define OP_IMUL                  132
#define OP_FMUL                  133
#define OP_SDIV                  135
#define OP_FDIV                  136
#define OP_SMOD                  139
#define OP_FNEGATE               127
#define OP_VECTOR_TIMES_SCALAR   142
#define OP_DOT                   148
#define OP_MATRIX_TIMES_VECTOR   145
#define OP_SELECT                169
#define OP_LOGICAL_OR            166
#define OP_LOGICAL_AND           167
#define OP_LOGICAL_NOT           168
#define OP_IEQUAL                170
#define OP_INOT_EQUAL            171
#define OP_SLESS_THAN            177
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
#define GLSL_FLOOR  8
#define GLSL_FRACT  10
#define GLSL_SIN    13
#define GLSL_COS    14
#define GLSL_SQRT   31
#define GLSL_EXP    27
#define GLSL_FMIN   37
#define GLSL_FMAX   40
#define GLSL_FCLAMP 43
#define GLSL_SCLAMP 45
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
#define FLOAT_MIN_DIRECT  0x3C23D70Au /* 0.01 */
#define FLOAT_FACE_X      0x3F19999Au /* 0.6 */
#define FLOAT_FACE_Z      0x3F4CCCCDu /* 0.8 */
#define FLOAT_GOLDEN_ANGLE   0x401998FFu /* 2.3999632 */
#define FLOAT_FOUR        0x40800000u /* 4.0 */
/* Number of points in the hemisphere that sky light rays cycle through */
#define SKY_POINTS        64
#define FLOAT_SKY_POINTS  0x42800000u /* 64.0 (also used for packing face direction with history length) */
#define FLOAT_TWO         0x40000000u /* 2.0 */
#define FLOAT_THREE       0x40400000u /* 3.0 */
#define FLOAT_INV_64      0x3C800000u /* 1/64 */
#define FLOAT_GOLDEN_RATIO_FRAC 0x3F1E377Au /* 0.618034 */
#define FLOAT_NEG_HALF    0xBF000000u /* -0.5 */
#define FLOAT_DEPTH_TOLERANCE 0x3CA3D70Au /* 0.02 */
#define FLOAT_MIN_WEIGHT  0x3A83126Fu /* 0.001 */
/* Interleaved gradient noise, see http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare */
#define FLOAT_IGN_X       0x3D897143u /* 0.06711056 */
#define FLOAT_IGN_Y       0x3BBF4590u /* 0.00583715 */
#define FLOAT_IGN_SCALE   0x4253EE82u /* 52.9829189 */

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

/* IDs needed for emitting a ray trace (see EmitTrace) */
struct TraceIds {
	unsigned int tBool, tInt, tUint, tFloat, tVec2, tVec4, tSampled, tUVPtr, tTablePtr, pParamsTable;
	unsigned int vParams, vTex, rayQuery, scene, cFlags, cMask, cNone, cCommitted, cZero, cOne, cHalf;
	unsigned int cIdx[4];
};

/* Equivalent GLSL (see Blocked() in FragmentShader's GLSL):
	if (cond && Blocked(origin, dir, tMax)) counter++;
*/
static void EmitTrace(const struct TraceIds* t, unsigned int cond, unsigned int origin, unsigned int dir,
					unsigned int tMax, unsigned int counter) {
	/* Default index buffer uses vertices 0,1,2 for the first triangle of a quad, then 2,3,0 */
	static const int evenVertex[3] = { 0, 1, 2 }, oddVertex[3] = { 2, 3, 0 };
	unsigned int traceLabel = Id(), traceMerge = Id();
	unsigned int loopHeader = Id(), loopCond = Id(), loopBody = Id(), loopContinue = Id(), loopMerge = Id();
	unsigned int confirmLabel = Id(), bodyMerge = Id(), proceed = Id();
	unsigned int custom = Id(), geom = Id(), prim = Id(), bary = Id(), entry = Id(), tablePtr = Id(), table = Id();
	unsigned int entryPtr = Id(), uvs = Id(), quad = Id(), first = Id(), oddBit = Id(), odd = Id();
	unsigned int bx = Id(), by = Id(), w0a = Id(), w0 = Id(), t0 = Id(), t1 = Id(), t2 = Id(), t01 = Id(), uvHit = Id();
	unsigned int sampler = Id(), texel = Id(), texAlpha = Id(), solid = Id();
	unsigned int hitType = Id(), hit = Id(), hitInt = Id(), cur = Id(), sum = Id();
	unsigned int uvK[3];
	int k;

	Op(OP_SELECTION_MERGE,    2, traceMerge, 0);
	Op(OP_BRANCH_CONDITIONAL, 3, cond, traceLabel, traceMerge);
	Op(OP_LABEL, 1, traceLabel);
	Op(OP_RAY_QUERY_INITIALIZE, 8, t->rayQuery, t->scene, t->cFlags, t->cMask, origin, t->cZero, dir, tMax);

	/* Opaque faces are hit automatically, but faces of blocks like leaves are only */
	/*  hit when the texture at the point the ray passes through isn't transparent */
	Op(OP_BRANCH, 1, loopHeader);
	Op(OP_LABEL,  1, loopHeader);
	Op(OP_LOOP_MERGE, 3, loopMerge, loopContinue, 0);
	Op(OP_BRANCH, 1, loopCond);
	Op(OP_LABEL,  1, loopCond);
	Op(OP_RAY_QUERY_PROCEED,  3, t->tBool, proceed, t->rayQuery);
	Op(OP_BRANCH_CONDITIONAL, 3, proceed, loopBody, loopMerge);
	Op(OP_LABEL,  1, loopBody);

	/* Each instance's custom index is where its geometries start in the table of vertex buffers */
	Op(OP_RAY_QUERY_GET_CUSTOM_INDEX,    4, t->tInt,  custom, t->rayQuery, t->cNone);
	Op(OP_RAY_QUERY_GET_GEOMETRY_INDEX,  4, t->tInt,  geom,   t->rayQuery, t->cNone);
	Op(OP_RAY_QUERY_GET_PRIMITIVE_INDEX, 4, t->tInt,  prim,   t->rayQuery, t->cNone);
	Op(OP_RAY_QUERY_GET_BARYCENTRICS,    4, t->tVec2, bary,   t->rayQuery, t->cNone);
	Op(OP_IADD, 4, t->tInt, entry, custom, geom);
	Op(OP_ACCESS_CHAIN, 4, t->pParamsTable, tablePtr, t->vParams, t->cIdx[0]);
	Op(OP_LOAD, 3, t->tTablePtr, table, tablePtr);
	Op(OP_PTR_ACCESS_CHAIN, 4, t->tTablePtr, entryPtr, table, entry);
	Op(OP_LOAD, 5, t->tUVPtr, uvs, entryPtr, MEMORY_ALIGNED, 8);

	Op(OP_SHIFT_RIGHT_LOGICAL, 4, t->tInt, quad,   prim, t->cIdx[1]);
	Op(OP_SHIFT_LEFT_LOGICAL,  4, t->tInt, first,  quad, t->cIdx[2]);
	Op(OP_BITWISE_AND,         4, t->tInt, oddBit, prim, t->cIdx[1]);
	Op(OP_INOT_EQUAL,          4, t->tBool, odd, oddBit, t->cIdx[0]);

	for (k = 0; k < 3; k++)
	{
		unsigned int sel = Id(), vertex = Id(), scaled3 = Id(), uvIndex = Id(), uvPtr = Id();
		uvK[k] = Id();
		Op(OP_SELECT, 5, t->tInt, sel, odd, t->cIdx[oddVertex[k]], t->cIdx[evenVertex[k]]);
		Op(OP_IADD,   4, t->tInt, vertex,  first, sel);
		Op(OP_IMUL,   4, t->tInt, scaled3, vertex, t->cIdx[3]);
		Op(OP_IADD,   4, t->tInt, uvIndex, scaled3, t->cIdx[2]);
		Op(OP_PTR_ACCESS_CHAIN, 4, t->tUVPtr, uvPtr, uvs, uvIndex);
		Op(OP_LOAD, 5, t->tVec2, uvK[k], uvPtr, MEMORY_ALIGNED, 8);
	}

	Op(OP_COMPOSITE_EXTRACT, 4, t->tFloat, bx, bary, 0);
	Op(OP_COMPOSITE_EXTRACT, 4, t->tFloat, by, bary, 1);
	Op(OP_FSUB, 4, t->tFloat, w0a, t->cOne, bx);
	Op(OP_FSUB, 4, t->tFloat, w0,  w0a,  by);
	Op(OP_VECTOR_TIMES_SCALAR, 4, t->tVec2, t0, uvK[0], w0);
	Op(OP_VECTOR_TIMES_SCALAR, 4, t->tVec2, t1, uvK[1], bx);
	Op(OP_VECTOR_TIMES_SCALAR, 4, t->tVec2, t2, uvK[2], by);
	Op(OP_FADD, 4, t->tVec2, t01,   t0,  t1);
	Op(OP_FADD, 4, t->tVec2, uvHit, t01, t2);

	Op(OP_LOAD, 3, t->tSampled, sampler, t->vTex);
	Op(OP_IMAGE_SAMPLE_EXPLICIT, 6, t->tVec4, texel, sampler, uvHit, IMAGE_OPERAND_LOD, t->cZero);
	Op(OP_COMPOSITE_EXTRACT, 4, t->tFloat, texAlpha, texel, 3);
	Op(OP_FORD_GREATER_EQUAL, 4, t->tBool, solid, texAlpha, t->cHalf);

	Op(OP_SELECTION_MERGE,    2, bodyMerge, 0);
	Op(OP_BRANCH_CONDITIONAL, 3, solid, confirmLabel, bodyMerge);
	Op(OP_LABEL, 1, confirmLabel);
	Op(OP_RAY_QUERY_CONFIRM, 1, t->rayQuery);
	Op(OP_BRANCH, 1, bodyMerge);
	Op(OP_LABEL,  1, bodyMerge);
	Op(OP_BRANCH, 1, loopContinue);
	Op(OP_LABEL,  1, loopContinue);
	Op(OP_BRANCH, 1, loopHeader);
	Op(OP_LABEL,  1, loopMerge);

	Op(OP_RAY_QUERY_GET_INTERSECTION_TYPE, 4, t->tUint, hitType, t->rayQuery, t->cCommitted);
	Op(OP_INOT_EQUAL, 4, t->tBool, hit,    hitType, t->cNone);
	Op(OP_SELECT,     5, t->tInt,  hitInt, hit, t->cIdx[1], t->cIdx[0]);
	Op(OP_LOAD,       3, t->tInt,  cur,    counter);
	Op(OP_IADD,       4, t->tInt,  sum,    cur, hitInt);
	Op(OP_STORE,  2, counter, sum);
	Op(OP_BRANCH, 1, traceMerge);
	Op(OP_LABEL,  1, traceMerge);
}

/* Equivalent GLSL:
#extension GL_EXT_ray_query : require                (ray traced shadows only)
#extension GL_EXT_buffer_reference : require          (ray traced shadows only)
layout(set = 0, binding = 0) uniform sampler2D tex;   (textured only)
layout(push_constant) uniform Constants {
	layout(offset = 72) float temporal;                (ray traced shadows only)
	layout(offset = 80) vec3 fogCol; float fogEnd; float fogDensity;
};
layout(location = 0) in vec4 in_col;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec4 out_col;

(ray traced shadows only)
layout(buffer_reference) buffer UVs   { vec2 uv[]; }; (vertices are 24 bytes, so 3 vec2s, with the UV being the 3rd)
layout(buffer_reference) buffer Table { UVs vbs[]; };
layout(set = 1, binding = 0) uniform accelerationStructureEXT scene;
layout(set = 1, binding = 1) uniform Params {
	Table table; vec3 camPos; float shadowDist; vec3 sunDir; float diffuseScale;
	vec3 shadowRatio; int samples; vec3 sunU; vec3 sunV;
	mat4 prevMVP; vec2 screenSize; int frame; int hasPrev; float maxHistory;
	float aoStrength; float aoRange;
};
layout(set = 1, binding = 2) uniform sampler2D history; (previous frame's out_history)
layout(location = 2) in vec3 in_world;
layout(location = 1) out vec4 out_history;            (sun visibility, view depth, face * 64 + history length, sky visibility)

bool Blocked(vec3 origin, vec3 dir, float tMax) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, scene, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, origin, 0.0, dir, tMax);
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
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

vec2 DiscPoint(int i, int count) { (Vogel/golden angle spiral)
	float r = sqrt((float(i) + 0.5) / float(count)), ang = float(i) * 2.3999632;
	return r * vec2(cos(ang), sin(ang));
}

void main() {
	vec3 dx = dFdx(in_world), dy = dFdy(in_world);       (ray traced shadows only)
	vec4 col = texture(tex, in_uv) * in_col;
	if (col.a < 0.5) discard;                            (alpha test only)

	(ray traced shadows only)
	vec3 toCam  = camPos - in_world;
	vec3 normal = normalize(cross(dx, dy));
	if (dot(normal, toCam) < 0.0) normal = -normal;
	vec3 a    = abs(normal); float big = max(a.x, max(a.y, a.z));
	bool axis = big > 0.9;
	if (axis) normal = sign(normal * vec3(greaterThanEqual(a, vec3(big))));

	float face   = !axis ? 1.0 : (abs(normal.y) > 0.5 ? (normal.y > 0.5 ? 1.0 : 0.5) : (abs(normal.x) > 0.5 ? 0.6 : 0.8));
	float faceID = !axis ? 3.0 : dot(normal, vec3(1, 2, 3)) + 3.0; (0-6, which way the face points)
	float direct = !axis ? 1.0 : clamp(dot(normal, sunDir) * diffuseScale, 0.0, 1.0);
	bool shaded  = in_col.a > 0.998; (alpha 254 = fully bright)
	bool doSun   = direct > 0.01, doSky = aoStrength > 0.0;
	float depth  = 1.0 / gl_FragCoord.w;

	float histVis = 1.0, histLen = 0.0, histSky = 1.0;
	if (temporal > 0.5 && hasPrev != 0) {
		vec4 prev = prevMVP * vec4(in_world, 1.0);
		vec2 pos  = (prev.xy / prev.w * vec2(0.5, -0.5) + 0.5) * screenSize - 0.5;
		vec2 f    = pos - floor(pos);
		float wSum = 0.0, vSum = 0.0, lSum = 0.0, sSum = 0.0;
		for (tap in (0,0), (1,0), (0,1), (1,1)) {
			vec4 h  = texelFetch(history, clamp(ivec2(floor(pos)) + tap, ivec2(0), ivec2(screenSize) - 1), 0);
			float hFace = floor(h.z / 64.0), hLen = h.z - hFace * 64.0;
			float w = (tap.x ? f.x : 1.0 - f.x) * (tap.y ? f.y : 1.0 - f.y);
			(only use history of the same surface, i.e. same depth and same face direction, that has history)
			if (abs(h.y - prev.w) >= prev.w * 0.02 + 0.05 || abs(hFace - faceID) > 0.5 || hLen < 0.5) w = 0.0;
			wSum += w; vSum += w * h.x; lSum += w * hLen; sSum += w * h.w;
		}
		if (wSum > 0.001) { histVis = vSum / wSum; histLen = lSum / wSum; histSky = sSum / wSum; }
	}

	float visible = 1.0, len = 0.0, sky = 1.0;
	if (shaded && length(toCam) < shadowDist && (doSun || doSky)) {
		vec3 origin = in_world + normal * 0.05;
		int rays    = histLen >= 4.0 ? 1 : 4;
		float ign   = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
		int sunStart = frame + int(ign * float(samples));
		int skyStart = frame + int(fract(ign + 0.618034) * 64.0);
		vec3 tangent = normalize(cross(abs(normal.y) > 0.9 ? vec3(1,0,0) : vec3(0,1,0), normal));
		vec3 bitangent = cross(normal, tangent);
		int sunHits = 0, skyHits = 0;

		for (int i = 0; i < rays; i++) {
			vec2 d = DiscPoint((sunStart + i * (samples / rays)) % samples, samples);
			if (doSun && Blocked(origin, normalize(sunDir + sunU * d.x + sunV * d.y), shadowDist)) sunHits++;

			(cosine weighted direction in the hemisphere above the face)
			vec2 s = DiscPoint((skyStart + i * (64 / rays)) % 64, 64);
			vec3 skyDir = tangent * s.x + bitangent * s.y + normal * sqrt(1.0 - dot(s, s));
			if (doSky && Blocked(origin, skyDir, aoRange)) skyHits++;
		}
		len     = min(histLen + 1.0, maxHistory);
		visible = histVis + ((1.0 - float(sunHits) / float(rays)) - histVis) * (samples < 4 ? 1.0 : 1.0 / len);
		sky     = histSky + ((1.0 - float(skyHits) / float(rays)) - histSky) / len;
	}
	out_history = vec4(visible, depth, faceID * 64.0 + len, sky);

	float ambient = 1.0 - aoStrength * (1.0 - sky);
	vec3 light    = shadowRatio * ambient + (1.0 - shadowRatio) * (direct * visible / face);
	if (shaded) col.rgb *= light;

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
	unsigned int vertexCol, world = 0, dx = 0, dy = 0;
	/* Ray traced shadows only */
	unsigned int tUint = 0, tBVec3 = 0, tIVec2 = 0, tMat4 = 0, tScene = 0, tRayQuery = 0, tUVPtr = 0, tTablePtr = 0, tParams = 0;
	unsigned int pScene = 0, pRayQuery = 0, pInVec3 = 0, pFuncFloat = 0, pFuncInt = 0, pParams = 0;
	unsigned int pParamsTable = 0, pParamsVec3 = 0, pParamsFloat = 0, pParamsInt = 0, pParamsMat4 = 0, pParamsVec2 = 0;
	unsigned int cFlags = 0, cMask = 0, cCommitted = 0, cNone = 0, cOffset = 0, cBright = 0, cAxis = 0, cMinDirect = 0;
	unsigned int cFaceX = 0, cFaceZ = 0, cGolden = 0, cVec3Zero = 0, cVec3One = 0, cVec3X = 0, cVec3Y = 0;
	unsigned int cFour = 0, cDepthTol = 0, cMinWeight = 0, cNegHalf = 0, cIgnX = 0, cIgnY = 0, cIgnScale = 0;
	unsigned int cNdcScale = 0, cVec2Half = 0, cIgnDir = 0, cIVec2Zero = 0, cIVec2One = 0;
	unsigned int cSkyPoints = 0, cSkyPointsF = 0, cGoldenFrac = 0, cFaceWeights = 0, cTwo = 0, cThree = 0, cInv64 = 0;
	unsigned int cTaps[4] = { 0, 0, 0, 0 };
	unsigned int vWorld = 0, vScene = 0, vParams = 0, vHistory = 0, vOutHistory = 0;
	unsigned int rayQuery = 0, vVisible = 0, vIndex = 0, vHits = 0, vSkyHits = 0;
	unsigned int vHistVis = 0, vHistLen = 0, vHistSky = 0, vLen = 0, vSky = 0;
	unsigned int cIdx[16];
	unsigned int iface[12];
	int i, count = 0, push = fog || rt, fogBase = rt ? 1 : 0, fragPos = fog || rt;

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
		tUint  = Id(); tBVec3 = Id(); tIVec2 = Id(); tMat4 = Id(); tScene = Id(); tRayQuery = Id(); tUVPtr = Id(); tTablePtr = Id(); tParams = Id();
		pScene = Id(); pRayQuery = Id(); pInVec3 = Id(); pFuncFloat = Id(); pFuncInt = Id(); pParams = Id();
		pParamsTable = Id(); pParamsVec3 = Id(); pParamsFloat = Id(); pParamsInt = Id(); pParamsMat4 = Id(); pParamsVec2 = Id();
		cFlags = Id(); cMask = Id(); cCommitted = Id(); cNone = Id(); cOffset = Id(); cBright = Id(); cAxis = Id(); cMinDirect = Id();
		cFaceX = Id(); cFaceZ = Id(); cGolden = Id(); cVec3Zero = Id(); cVec3One = Id(); cVec3X = Id(); cVec3Y = Id();
		cFour  = Id(); cDepthTol = Id(); cMinWeight = Id(); cNegHalf = Id(); cIgnX = Id(); cIgnY = Id(); cIgnScale = Id();
		cNdcScale = Id(); cVec2Half = Id(); cIgnDir = Id(); cIVec2Zero = Id(); cIVec2One = Id();
		cSkyPoints = Id(); cSkyPointsF = Id(); cGoldenFrac = Id(); cFaceWeights = Id(); cTwo = Id(); cThree = Id(); cInv64 = Id();
		for (i = 0; i < 4; i++) cTaps[i] = Id();
		vWorld = Id(); vScene = Id(); vParams = Id(); vHistory = Id(); vOutHistory = Id();
		rayQuery = Id(); vVisible = Id(); vIndex = Id(); vHits = Id(); vSkyHits = Id();
		vHistVis = Id(); vHistLen = Id(); vHistSky = Id(); vLen = Id(); vSky = Id();
		for (i = 3; i < 16; i++) cIdx[i] = Id();
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
	if (fragPos)  iface[count++] = vFragCoord;
	iface[count++] = vOut;
	if (rt) {
		iface[count++] = vWorld;
		iface[count++] = vOutHistory;
		iface[count++] = vScene;
		iface[count++] = vParams;
		iface[count++] = vHistory;
		iface[count++] = vPush;
		if (textured) iface[count++] = vTex;
	}
	EntryPoint(MODEL_FRAGMENT, main_, count, iface);
	Op(OP_EXECUTION_MODE, 2, main_, MODE_ORIGIN_UPPER_LEFT);

	Op(OP_DECORATE, 3, vCol, DECORATION_LOCATION, 0);
	if (textured) Op(OP_DECORATE, 3, vUV, DECORATION_LOCATION, 1);
	if (fragPos)  Op(OP_DECORATE, 3, vFragCoord, DECORATION_BUILTIN, BUILTIN_FRAG_COORD);
	Op(OP_DECORATE, 3, vOut, DECORATION_LOCATION, 0);
	if (textured) {
		Op(OP_DECORATE, 3, vTex, DECORATION_DESCRIPTOR_SET, 0);
		Op(OP_DECORATE, 3, vTex, DECORATION_BINDING, 0);
	}
	if (rt) {
		Op(OP_DECORATE, 2, tPush, DECORATION_BLOCK);
		Op(OP_MEMBER_DECORATE, 4, tPush, 0, DECORATION_OFFSET, 72);
		Op(OP_MEMBER_DECORATE, 4, tPush, 1, DECORATION_OFFSET, 80);
		Op(OP_MEMBER_DECORATE, 4, tPush, 2, DECORATION_OFFSET, 92);
		Op(OP_MEMBER_DECORATE, 4, tPush, 3, DECORATION_OFFSET, 96);
	} else if (fog) {
		Op(OP_DECORATE, 2, tPush, DECORATION_BLOCK);
		Op(OP_MEMBER_DECORATE, 4, tPush, 0, DECORATION_OFFSET, 80);
		Op(OP_MEMBER_DECORATE, 4, tPush, 1, DECORATION_OFFSET, 92);
		Op(OP_MEMBER_DECORATE, 4, tPush, 2, DECORATION_OFFSET, 96);
	}
	if (rt) {
		Op(OP_DECORATE, 3, vWorld, DECORATION_LOCATION, 2);
		Op(OP_DECORATE, 3, vOutHistory, DECORATION_LOCATION, 1);
		Op(OP_DECORATE, 3, vScene, DECORATION_DESCRIPTOR_SET, 1);
		Op(OP_DECORATE, 3, vScene, DECORATION_BINDING, 0);
		Op(OP_DECORATE, 3, vHistory, DECORATION_DESCRIPTOR_SET, 1);
		Op(OP_DECORATE, 3, vHistory, DECORATION_BINDING, 2);
		/* Vertices are 24 bytes, so are read as 3 vec2s, with the UV being the 3rd */
		Op(OP_DECORATE, 3, tUVPtr,    DECORATION_ARRAY_STRIDE, 8);
		Op(OP_DECORATE, 3, tTablePtr, DECORATION_ARRAY_STRIDE, 8);
		/* NOTE: Layout must match struct RTParams in Graphics_Vulkan.c */
		Op(OP_DECORATE, 2, tParams, DECORATION_BLOCK);
		Op(OP_MEMBER_DECORATE, 4, tParams, 0,  DECORATION_OFFSET, 0);
		Op(OP_MEMBER_DECORATE, 4, tParams, 1,  DECORATION_OFFSET, 16);
		Op(OP_MEMBER_DECORATE, 4, tParams, 2,  DECORATION_OFFSET, 28);
		Op(OP_MEMBER_DECORATE, 4, tParams, 3,  DECORATION_OFFSET, 32);
		Op(OP_MEMBER_DECORATE, 4, tParams, 4,  DECORATION_OFFSET, 44);
		Op(OP_MEMBER_DECORATE, 4, tParams, 5,  DECORATION_OFFSET, 48);
		Op(OP_MEMBER_DECORATE, 4, tParams, 6,  DECORATION_OFFSET, 60);
		Op(OP_MEMBER_DECORATE, 4, tParams, 7,  DECORATION_OFFSET, 64);
		Op(OP_MEMBER_DECORATE, 4, tParams, 8,  DECORATION_OFFSET, 80);
		Op(OP_MEMBER_DECORATE, 3, tParams, 9,  DECORATION_COL_MAJOR);
		Op(OP_MEMBER_DECORATE, 4, tParams, 9,  DECORATION_OFFSET, 96);
		Op(OP_MEMBER_DECORATE, 4, tParams, 9,  DECORATION_MATRIX_STRIDE, 16);
		Op(OP_MEMBER_DECORATE, 4, tParams, 10, DECORATION_OFFSET, 160);
		Op(OP_MEMBER_DECORATE, 4, tParams, 11, DECORATION_OFFSET, 168);
		Op(OP_MEMBER_DECORATE, 4, tParams, 12, DECORATION_OFFSET, 172);
		Op(OP_MEMBER_DECORATE, 4, tParams, 13, DECORATION_OFFSET, 176);
		Op(OP_MEMBER_DECORATE, 4, tParams, 14, DECORATION_OFFSET, 180);
		Op(OP_MEMBER_DECORATE, 4, tParams, 15, DECORATION_OFFSET, 184);
		Op(OP_DECORATE, 3, vParams, DECORATION_DESCRIPTOR_SET, 1);
		Op(OP_DECORATE, 3, vParams, DECORATION_BINDING, 1);
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
		Op(OP_TYPE_STRUCT,  5, tPush, tFloat, tVec3, tFloat, tFloat);
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
		Op(OP_TYPE_VECTOR,  3, tBVec3, tBool, 3);
		Op(OP_TYPE_VECTOR,  3, tIVec2, tInt,  2);
		Op(OP_TYPE_MATRIX,  3, tMat4,  tVec4, 4);
		Op(OP_TYPE_ACCELERATION_STRUCTURE, 1, tScene);
		Op(OP_TYPE_RAY_QUERY,              1, tRayQuery);
		Op(OP_TYPE_POINTER, 3, tUVPtr,     STORAGE_PHYSICAL_BUFFER, tVec2);
		Op(OP_TYPE_POINTER, 3, tTablePtr,  STORAGE_PHYSICAL_BUFFER, tUVPtr);
		Op(OP_TYPE_STRUCT, 17, tParams,    tTablePtr, tVec3, tFloat, tVec3, tFloat, tVec3, tInt, tVec3, tVec3,
											tMat4, tVec2, tInt, tInt, tFloat, tFloat, tFloat);
		Op(OP_TYPE_POINTER, 3, pScene,     STORAGE_UNIFORM_CONSTANT, tScene);
		Op(OP_TYPE_POINTER, 3, pRayQuery,  STORAGE_FUNCTION, tRayQuery);
		Op(OP_TYPE_POINTER, 3, pInVec3,    STORAGE_INPUT,    tVec3);
		Op(OP_TYPE_POINTER, 3, pFuncFloat, STORAGE_FUNCTION, tFloat);
		Op(OP_TYPE_POINTER, 3, pFuncInt,   STORAGE_FUNCTION, tInt);
		Op(OP_TYPE_POINTER, 3, pParams,      STORAGE_UNIFORM, tParams);
		Op(OP_TYPE_POINTER, 3, pParamsTable, STORAGE_UNIFORM, tTablePtr);
		Op(OP_TYPE_POINTER, 3, pParamsVec3,  STORAGE_UNIFORM, tVec3);
		Op(OP_TYPE_POINTER, 3, pParamsFloat, STORAGE_UNIFORM, tFloat);
		Op(OP_TYPE_POINTER, 3, pParamsInt,   STORAGE_UNIFORM, tInt);
		Op(OP_TYPE_POINTER, 3, pParamsMat4,  STORAGE_UNIFORM, tMat4);
		Op(OP_TYPE_POINTER, 3, pParamsVec2,  STORAGE_UNIFORM, tVec2);
	}
	Op(OP_CONSTANT, 3, tFloat, cZero, FLOAT_ZERO);
	Op(OP_CONSTANT, 3, tFloat, cOne,  FLOAT_ONE);
	Op(OP_CONSTANT, 3, tFloat, cHalf, FLOAT_HALF);
	Op(OP_CONSTANT, 3, tInt,   cInt0, 0);
	Op(OP_CONSTANT, 3, tInt,   cInt1, 1);
	Op(OP_CONSTANT, 3, tInt,   cInt2, 2);
	if (rt) {
		for (i = 3; i < 16; i++) Op(OP_CONSTANT, 3, tInt, cIdx[i], i);
		Op(OP_CONSTANT, 3, tUint,  cFlags,     RAY_FLAGS_SHADOW);
		Op(OP_CONSTANT, 3, tUint,  cMask,      0xFF);
		Op(OP_CONSTANT, 3, tUint,  cCommitted, RAY_INTERSECTION_COMMITTED);
		Op(OP_CONSTANT, 3, tUint,  cNone,      0);
		Op(OP_CONSTANT, 3, tFloat, cOffset,    FLOAT_RAY_OFFSET);
		Op(OP_CONSTANT, 3, tFloat, cBright,    FLOAT_FULLBRIGHT);
		Op(OP_CONSTANT, 3, tFloat, cAxis,      FLOAT_AXIS_NORMAL);
		Op(OP_CONSTANT, 3, tFloat, cMinDirect, FLOAT_MIN_DIRECT);
		Op(OP_CONSTANT, 3, tFloat, cFaceX,     FLOAT_FACE_X);
		Op(OP_CONSTANT, 3, tFloat, cFaceZ,     FLOAT_FACE_Z);
		Op(OP_CONSTANT, 3, tFloat, cGolden,    FLOAT_GOLDEN_ANGLE);
		Op(OP_CONSTANT, 3, tFloat, cFour,      FLOAT_FOUR);
		Op(OP_CONSTANT, 3, tFloat, cDepthTol,  FLOAT_DEPTH_TOLERANCE);
		Op(OP_CONSTANT, 3, tFloat, cMinWeight, FLOAT_MIN_WEIGHT);
		Op(OP_CONSTANT, 3, tFloat, cNegHalf,   FLOAT_NEG_HALF);
		Op(OP_CONSTANT, 3, tFloat, cIgnX,      FLOAT_IGN_X);
		Op(OP_CONSTANT, 3, tFloat, cIgnY,      FLOAT_IGN_Y);
		Op(OP_CONSTANT, 3, tFloat, cIgnScale,  FLOAT_IGN_SCALE);
		Op(OP_CONSTANT, 3, tInt,   cSkyPoints,  SKY_POINTS);
		Op(OP_CONSTANT, 3, tFloat, cSkyPointsF, FLOAT_SKY_POINTS);
		Op(OP_CONSTANT, 3, tFloat, cGoldenFrac, FLOAT_GOLDEN_RATIO_FRAC);
		Op(OP_CONSTANT, 3, tFloat, cTwo,        FLOAT_TWO);
		Op(OP_CONSTANT, 3, tFloat, cThree,      FLOAT_THREE);
		Op(OP_CONSTANT, 3, tFloat, cInv64,      FLOAT_INV_64);
		Op(OP_CONSTANT_COMPOSITE, 5, tVec3, cVec3Zero, cZero, cZero, cZero);
		Op(OP_CONSTANT_COMPOSITE, 5, tVec3, cVec3One,  cOne,  cOne,  cOne);
		Op(OP_CONSTANT_COMPOSITE, 5, tVec3, cVec3X,    cOne,  cZero, cZero);
		Op(OP_CONSTANT_COMPOSITE, 5, tVec3, cVec3Y,    cZero, cOne,  cZero);
		Op(OP_CONSTANT_COMPOSITE, 5, tVec3, cFaceWeights, cOne, cTwo, cThree);
		Op(OP_CONSTANT_COMPOSITE, 4, tVec2, cNdcScale, cHalf, cNegHalf);
		Op(OP_CONSTANT_COMPOSITE, 4, tVec2, cVec2Half, cHalf, cHalf);
		Op(OP_CONSTANT_COMPOSITE, 4, tVec2, cIgnDir,   cIgnX, cIgnY);
		Op(OP_CONSTANT_COMPOSITE, 4, tIVec2, cIVec2Zero, cInt0, cInt0);
		Op(OP_CONSTANT_COMPOSITE, 4, tIVec2, cIVec2One,  cInt1, cInt1);
		for (i = 0; i < 4; i++)
		{
			Op(OP_CONSTANT_COMPOSITE, 4, tIVec2, cTaps[i], cIdx[i & 1], cIdx[i >> 1]);
		}
	}

	Op(OP_VARIABLE, 3, pInVec4, vCol, STORAGE_INPUT);
	if (textured) Op(OP_VARIABLE, 3, pInVec2, vUV, STORAGE_INPUT);
	if (fragPos)  Op(OP_VARIABLE, 3, pInVec4, vFragCoord, STORAGE_INPUT);
	Op(OP_VARIABLE, 3, pOutVec4, vOut, STORAGE_OUTPUT);
	if (textured) Op(OP_VARIABLE, 3, pSampled, vTex, STORAGE_UNIFORM_CONSTANT);
	if (push)     Op(OP_VARIABLE, 3, pPush, vPush, STORAGE_PUSH_CONSTANT);
	if (rt) {
		Op(OP_VARIABLE, 3, pInVec3,  vWorld,      STORAGE_INPUT);
		Op(OP_VARIABLE, 3, pOutVec4, vOutHistory, STORAGE_OUTPUT);
		Op(OP_VARIABLE, 3, pScene,   vScene,      STORAGE_UNIFORM_CONSTANT);
		Op(OP_VARIABLE, 3, pParams,  vParams,     STORAGE_UNIFORM);
		Op(OP_VARIABLE, 3, pSampled, vHistory,    STORAGE_UNIFORM_CONSTANT);
	}

	Op(OP_FUNCTION, 4, tVoid, main_, 0, tFunc);
	Op(OP_LABEL,    1, label);
	/* Function variables must be declared at the very start of the first block */
	if (rt) {
		Op(OP_VARIABLE, 3, pRayQuery,  rayQuery, STORAGE_FUNCTION);
		Op(OP_VARIABLE, 3, pFuncFloat, vVisible, STORAGE_FUNCTION);
		Op(OP_VARIABLE, 3, pFuncInt,   vIndex,   STORAGE_FUNCTION);
		Op(OP_VARIABLE, 3, pFuncInt,   vHits,    STORAGE_FUNCTION);
		Op(OP_VARIABLE, 3, pFuncInt,   vSkyHits, STORAGE_FUNCTION);
		Op(OP_VARIABLE, 3, pFuncFloat, vHistVis, STORAGE_FUNCTION);
		Op(OP_VARIABLE, 3, pFuncFloat, vHistLen, STORAGE_FUNCTION);
		Op(OP_VARIABLE, 3, pFuncFloat, vHistSky, STORAGE_FUNCTION);
		Op(OP_VARIABLE, 3, pFuncFloat, vLen,     STORAGE_FUNCTION);
		Op(OP_VARIABLE, 3, pFuncFloat, vSky,     STORAGE_FUNCTION);
	}
	Op(OP_LOAD,     3, tVec4, col, vCol);
	vertexCol = col;

	/* Derivatives are undefined after discarding pixels (since neighbouring pixels in the */
	/*  2x2 quad might have been discarded), so the change in position is calculated first */
	if (rt) {
		world = Id(); dx = Id(); dy = Id();
		Op(OP_LOAD, 3, tVec3, world, vWorld);
		Op(OP_DPDX, 3, tVec3, dx, world);
		Op(OP_DPDY, 3, tVec3, dy, world);
	}

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
		unsigned int cam, dist, sun, diffuseScale, ratio, samples, sunU, sunV, prevMVP, screen, frameNum, hasPrev, maxHistory;
		unsigned int aoStrength, aoRange, temporal;
		unsigned int toCam, crossed, normal, toCamDot, flip, negated, flipped;
		unsigned int absN, ax, ay, az, bigYZ, big, bigs, isBig, mask, masked, snapped, axisAligned, n;
		unsigned int ny, nyAbs, nxAbs, isY, isTop, isX, faceY, faceXZ, faceAxis, face, faceDot, faceAxisID, faceID;
		unsigned int ndl, ndlScaled, directAxis, direct, vertexAlpha, shadedCol, len, inRange, doSun, doSky, anyRay, doRayA, doRay;
		unsigned int fragPosV, fragW, viewDepth, isTemporal, hasPrevB, useHistory, histLabel, histMerge;
		unsigned int rayLabel, rayMerge, visible, newLen, sky, history, faceShifted, faceLen;
		unsigned int skyLost, skyScaled, ambient, ambientRatio, oneMinus, lightK1, lightK, lightAdd, light, rgbCol, lit, litRgb, litCol;
		struct TraceIds trace;

		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsVec3,  paramPtr, vParams, cIdx[1]);  cam          = Id(); Op(OP_LOAD, 3, tVec3,  cam,          paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsFloat, paramPtr, vParams, cIdx[2]);  dist         = Id(); Op(OP_LOAD, 3, tFloat, dist,         paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsVec3,  paramPtr, vParams, cIdx[3]);  sun          = Id(); Op(OP_LOAD, 3, tVec3,  sun,          paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsFloat, paramPtr, vParams, cIdx[4]);  diffuseScale = Id(); Op(OP_LOAD, 3, tFloat, diffuseScale, paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsVec3,  paramPtr, vParams, cIdx[5]);  ratio        = Id(); Op(OP_LOAD, 3, tVec3,  ratio,        paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsInt,   paramPtr, vParams, cIdx[6]);  samples      = Id(); Op(OP_LOAD, 3, tInt,   samples,      paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsVec3,  paramPtr, vParams, cIdx[7]);  sunU         = Id(); Op(OP_LOAD, 3, tVec3,  sunU,         paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsVec3,  paramPtr, vParams, cIdx[8]);  sunV         = Id(); Op(OP_LOAD, 3, tVec3,  sunV,         paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsMat4,  paramPtr, vParams, cIdx[9]);  prevMVP      = Id(); Op(OP_LOAD, 3, tMat4,  prevMVP,      paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsVec2,  paramPtr, vParams, cIdx[10]); screen       = Id(); Op(OP_LOAD, 3, tVec2,  screen,       paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsInt,   paramPtr, vParams, cIdx[11]); frameNum     = Id(); Op(OP_LOAD, 3, tInt,   frameNum,     paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsInt,   paramPtr, vParams, cIdx[12]); hasPrev      = Id(); Op(OP_LOAD, 3, tInt,   hasPrev,      paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsFloat, paramPtr, vParams, cIdx[13]); maxHistory   = Id(); Op(OP_LOAD, 3, tFloat, maxHistory,   paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsFloat, paramPtr, vParams, cIdx[14]); aoStrength   = Id(); Op(OP_LOAD, 3, tFloat, aoStrength,   paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pParamsFloat, paramPtr, vParams, cIdx[15]); aoRange      = Id(); Op(OP_LOAD, 3, tFloat, aoRange,      paramPtr);
		paramPtr = Id(); Op(OP_ACCESS_CHAIN, 4, pPushFloat,   paramPtr, vPush,   cIdx[0]);  temporal     = Id(); Op(OP_LOAD, 3, tFloat, temporal,     paramPtr);

		toCam = Id(); crossed = Id(); normal = Id(); toCamDot = Id(); flip = Id(); negated = Id(); flipped = Id();
		Op(OP_FSUB, 4, tVec3, toCam, cam, world);

		/* Faces are flat, so the normal can be calculated from the change in position across pixels */
		Op(OP_EXT_INST, 6, tVec3, crossed, ext, GLSL_CROSS, dx, dy);
		Op(OP_EXT_INST, 5, tVec3, normal,  ext, GLSL_NORMALIZE, crossed);
		Op(OP_DOT,      4, tFloat, toCamDot, normal, toCam);
		Op(OP_FORD_LESS_THAN, 4, tBool, flip, toCamDot, cZero);
		Op(OP_FNEGATE,  3, tVec3, negated, normal);
		Op(OP_SELECT,   5, tVec3, flipped, flip, negated, normal);

		/* The calculated normal is slightly inaccurate, so block faces (which are axis aligned) */
		/*  have their normal snapped to the closest axis. Sprites aren't, so keep the calculated normal */
		absN = Id(); ax = Id(); ay = Id(); az = Id(); bigYZ = Id(); big = Id(); bigs = Id();
		isBig = Id(); mask = Id(); masked = Id(); snapped = Id(); axisAligned = Id(); n = Id();
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
		Op(OP_SELECT,   5, tVec3, n, axisAligned, snapped, flipped);

		/* Vertex colours already include the classic shading of each face (top 1.0, Z sides 0.8, X sides 0.6, bottom 0.5) */
		ny = Id(); nyAbs = Id(); nxAbs = Id(); isY = Id(); isTop = Id(); isX = Id();
		faceY = Id(); faceXZ = Id(); faceAxis = Id(); face = Id();
		Op(OP_COMPOSITE_EXTRACT, 4, tFloat, ny, n, 1);
		Op(OP_EXT_INST, 5, tFloat, nyAbs, ext, GLSL_FABS, ny);
		Op(OP_COMPOSITE_EXTRACT, 4, tFloat, nxAbs, absN, 0);
		Op(OP_FORD_GREATER_THAN, 4, tBool, isY,   nyAbs, cHalf);
		Op(OP_FORD_GREATER_THAN, 4, tBool, isTop, ny,    cHalf);
		Op(OP_FORD_GREATER_THAN, 4, tBool, isX,   nxAbs, cHalf);
		Op(OP_SELECT, 5, tFloat, faceY,    isTop, cOne,   cHalf);
		Op(OP_SELECT, 5, tFloat, faceXZ,   isX,   cFaceX, cFaceZ);
		Op(OP_SELECT, 5, tFloat, faceAxis, isY,   faceY,  faceXZ);
		Op(OP_SELECT, 5, tFloat, face,     axisAligned, faceAxis, cOne);

		/* Which way the face points (0 to 6, 3 for sprites), so the history of other faces is ignored */
		faceDot = Id(); faceAxisID = Id(); faceID = Id();
		Op(OP_DOT,    4, tFloat, faceDot,    n, cFaceWeights);
		Op(OP_FADD,   4, tFloat, faceAxisID, faceDot, cThree);
		Op(OP_SELECT, 5, tFloat, faceID,     axisAligned, faceAxisID, cThree);

		/* How directly sunlight hits the face, scaled so the face most towards the sun gets full sunlight */
		/*  (sprites are lit the same from both sides) */
		ndl = Id(); ndlScaled = Id(); directAxis = Id(); direct = Id();
		Op(OP_DOT,  4, tFloat, ndl, n, sun);
		Op(OP_FMUL, 4, tFloat, ndlScaled, ndl, diffuseScale);
		Op(OP_EXT_INST, 7, tFloat, directAxis, ext, GLSL_FCLAMP, ndlScaled, cZero, cOne);
		Op(OP_SELECT,   5, tFloat, direct, axisAligned, directAxis, cOne);

		/* Fully bright faces (alpha 254) aren't shaded, and faces beyond the shadow distance aren't shadowed */
		vertexAlpha = Id(); shadedCol = Id(); len = Id(); inRange = Id(); doSun = Id(); doSky = Id(); anyRay = Id();
		doRayA = Id(); doRay = Id();
		Op(OP_COMPOSITE_EXTRACT, 4, tFloat, vertexAlpha, vertexCol, 3);
		Op(OP_FORD_GREATER_THAN, 4, tBool, shadedCol, vertexAlpha, cBright);
		Op(OP_EXT_INST, 5, tFloat, len, ext, GLSL_LENGTH, toCam);
		Op(OP_FORD_LESS_THAN,    4, tBool, inRange, len, dist);
		Op(OP_FORD_GREATER_THAN, 4, tBool, doSun,   direct, cMinDirect);
		Op(OP_FORD_GREATER_THAN, 4, tBool, doSky,   aoStrength, cZero);
		Op(OP_LOGICAL_OR,  4, tBool, anyRay, doSun, doSky);
		Op(OP_LOGICAL_AND, 4, tBool, doRayA, shadedCol, inRange);
		Op(OP_LOGICAL_AND, 4, tBool, doRay,  doRayA, anyRay);

		/* Distance from the camera along the view direction (i.e. clip space W) */
		fragPosV = Id(); fragW = Id(); viewDepth = Id();
		Op(OP_LOAD, 3, tVec4, fragPosV, vFragCoord);
		Op(OP_COMPOSITE_EXTRACT, 4, tFloat, fragW, fragPosV, 3);
		Op(OP_FDIV, 4, tFloat, viewDepth, cOne, fragW);

		/* Find where this point was on screen in the previous frame, then blend the 4 pixels of the */
		/*  previous frame's shadow history around there, ignoring pixels that were of a different surface */
		/*  (i.e. at a different depth). Translucent faces don't use the history (see PrepareDraw) */
		isTemporal = Id(); hasPrevB = Id(); useHistory = Id(); histLabel = Id(); histMerge = Id();
		Op(OP_STORE, 2, vHistVis, cOne);
		Op(OP_STORE, 2, vHistLen, cZero);
		Op(OP_STORE, 2, vHistSky, cOne);
		Op(OP_FORD_GREATER_THAN, 4, tBool, isTemporal, temporal, cHalf);
		Op(OP_INOT_EQUAL,        4, tBool, hasPrevB,   hasPrev,  cInt0);
		Op(OP_LOGICAL_AND,       4, tBool, useHistory, isTemporal, hasPrevB);
		Op(OP_SELECTION_MERGE,    2, histMerge, 0);
		Op(OP_BRANCH_CONDITIONAL, 3, useHistory, histLabel, histMerge);
		Op(OP_LABEL, 1, histLabel);
		{
			unsigned int world4, prev, pw, pxy, invW, ndc, ndcScaled, prevUV, prevPix, pos, base, frac, fx, fy, gx, gy;
			unsigned int baseI, sizeI, maxI, tolA, tol, histSampled, histImage, valid, hvAvg, hlAvg, hsAvg, hvSel, hlSel, hsSel;
			unsigned int wSum = cZero, vSum = cZero, lSum = cZero, sSum = cZero;
			int t;

			world4 = Id(); prev = Id(); pw = Id(); pxy = Id(); invW = Id(); ndc = Id(); ndcScaled = Id(); prevUV = Id();
			prevPix = Id(); pos = Id(); base = Id(); frac = Id(); fx = Id(); fy = Id(); gx = Id(); gy = Id();
			baseI = Id(); sizeI = Id(); maxI = Id(); tolA = Id(); tol = Id(); histSampled = Id(); histImage = Id();
			Op(OP_COMPOSITE_CONSTRUCT, 4, tVec4, world4, world, cOne);
			Op(OP_MATRIX_TIMES_VECTOR, 4, tVec4, prev, prevMVP, world4);
			Op(OP_COMPOSITE_EXTRACT,   4, tFloat, pw, prev, 3);
			Op(OP_VECTOR_SHUFFLE,      6, tVec2, pxy, prev, prev, 0, 1);
			Op(OP_FDIV, 4, tFloat, invW, cOne, pw);
			Op(OP_VECTOR_TIMES_SCALAR, 4, tVec2, ndc, pxy, invW);
			/* Viewport is flipped vertically (see Gfx_SetViewport) */
			Op(OP_FMUL, 4, tVec2, ndcScaled, ndc, cNdcScale);
			Op(OP_FADD, 4, tVec2, prevUV,  ndcScaled, cVec2Half);
			Op(OP_FMUL, 4, tVec2, prevPix, prevUV, screen);
			Op(OP_FSUB, 4, tVec2, pos,     prevPix, cVec2Half);
			Op(OP_EXT_INST, 5, tVec2, base, ext, GLSL_FLOOR, pos);
			Op(OP_FSUB, 4, tVec2, frac, pos, base);
			Op(OP_COMPOSITE_EXTRACT, 4, tFloat, fx, frac, 0);
			Op(OP_COMPOSITE_EXTRACT, 4, tFloat, fy, frac, 1);
			Op(OP_FSUB, 4, tFloat, gx, cOne, fx);
			Op(OP_FSUB, 4, tFloat, gy, cOne, fy);
			Op(OP_CONVERT_F_TO_S, 3, tIVec2, baseI, base);
			Op(OP_CONVERT_F_TO_S, 3, tIVec2, sizeI, screen);
			Op(OP_ISUB, 4, tIVec2, maxI, sizeI, cIVec2One);
			Op(OP_FMUL, 4, tFloat, tolA, pw, cDepthTol);
			Op(OP_FADD, 4, tFloat, tol,  tolA, cOffset);
			Op(OP_LOAD, 3, tSampled, histSampled, vHistory);
			Op(OP_IMAGE, 3, tImage, histImage, histSampled);

			for (t = 0; t < 4; t++)
			{
				unsigned int p = Id(), pc = Id(), h = Id(), hv = Id(), hd = Id(), hl = Id(), hs = Id(), diff = Id(), absDiff = Id(), same = Id();
				unsigned int wb = Id(), wt = Id(), wSum2 = Id(), wv = Id(), vSum2 = Id(), wl = Id(), lSum2 = Id(), ws = Id(), sSum2 = Id();
				unsigned int hScaled = Id(), hFace = Id(), hFace64 = Id(), hLen = Id(), faceDiff = Id(), faceAbs = Id(), sameFace = Id();
				unsigned int hasLen = Id(), sameDepthFace = Id(), useTap = Id();
				Op(OP_IADD, 4, tIVec2, p, baseI, cTaps[t]);
				Op(OP_EXT_INST, 7, tIVec2, pc, ext, GLSL_SCLAMP, p, cIVec2Zero, maxI);
				Op(OP_IMAGE_FETCH, 6, tVec4, h, histImage, pc, IMAGE_OPERAND_LOD, cInt0);
				Op(OP_COMPOSITE_EXTRACT, 4, tFloat, hv, h, 0);
				Op(OP_COMPOSITE_EXTRACT, 4, tFloat, hd, h, 1);
				Op(OP_COMPOSITE_EXTRACT, 4, tFloat, hl, h, 2);
				Op(OP_COMPOSITE_EXTRACT, 4, tFloat, hs, h, 3);
				/* Only use history of the same surface (i.e. same depth and same face direction) that has history */
				Op(OP_FMUL, 4, tFloat, hScaled, hl, cInv64);
				Op(OP_EXT_INST, 5, tFloat, hFace, ext, GLSL_FLOOR, hScaled);
				Op(OP_FMUL, 4, tFloat, hFace64, hFace, cSkyPointsF);
				Op(OP_FSUB, 4, tFloat, hLen, hl, hFace64);
				Op(OP_FSUB, 4, tFloat, diff, hd, pw);
				Op(OP_EXT_INST, 5, tFloat, absDiff, ext, GLSL_FABS, diff);
				Op(OP_FORD_LESS_THAN, 4, tBool, same, absDiff, tol);
				Op(OP_FSUB, 4, tFloat, faceDiff, hFace, faceID);
				Op(OP_EXT_INST, 5, tFloat, faceAbs, ext, GLSL_FABS, faceDiff);
				Op(OP_FORD_LESS_THAN,    4, tBool, sameFace, faceAbs, cHalf);
				Op(OP_FORD_GREATER_THAN, 4, tBool, hasLen,   hLen,    cHalf);
				Op(OP_LOGICAL_AND, 4, tBool, sameDepthFace, same, sameFace);
				Op(OP_LOGICAL_AND, 4, tBool, useTap,        sameDepthFace, hasLen);
				Op(OP_FMUL, 4, tFloat, wb, (t & 1) ? fx : gx, (t >> 1) ? fy : gy);
				Op(OP_SELECT, 5, tFloat, wt, useTap, wb, cZero);
				Op(OP_FADD, 4, tFloat, wSum2, wSum, wt);
				Op(OP_FMUL, 4, tFloat, wv, wt, hv);
				Op(OP_FADD, 4, tFloat, vSum2, vSum, wv);
				Op(OP_FMUL, 4, tFloat, wl, wt, hLen);
				Op(OP_FADD, 4, tFloat, lSum2, lSum, wl);
				Op(OP_FMUL, 4, tFloat, ws, wt, hs);
				Op(OP_FADD, 4, tFloat, sSum2, sSum, ws);
				wSum = wSum2; vSum = vSum2; lSum = lSum2; sSum = sSum2;
			}

			valid = Id(); hvAvg = Id(); hlAvg = Id(); hsAvg = Id(); hvSel = Id(); hlSel = Id(); hsSel = Id();
			Op(OP_FORD_GREATER_THAN, 4, tBool, valid, wSum, cMinWeight);
			Op(OP_FDIV, 4, tFloat, hvAvg, vSum, wSum);
			Op(OP_FDIV, 4, tFloat, hlAvg, lSum, wSum);
			Op(OP_FDIV, 4, tFloat, hsAvg, sSum, wSum);
			Op(OP_SELECT, 5, tFloat, hvSel, valid, hvAvg, cOne);
			Op(OP_SELECT, 5, tFloat, hlSel, valid, hlAvg, cZero);
			Op(OP_SELECT, 5, tFloat, hsSel, valid, hsAvg, cOne);
			Op(OP_STORE,  2, vHistVis, hvSel);
			Op(OP_STORE,  2, vHistLen, hlSel);
			Op(OP_STORE,  2, vHistSky, hsSel);
			Op(OP_BRANCH, 1, histMerge);
		}
		Op(OP_LABEL, 1, histMerge);

		Op(OP_STORE, 2, vVisible, cOne);
		Op(OP_STORE, 2, vLen,     cZero);
		Op(OP_STORE, 2, vSky,     cOne);
		rayLabel = Id(); rayMerge = Id();
		Op(OP_SELECTION_MERGE,    2, rayMerge, 0);
		Op(OP_BRANCH_CONDITIONAL, 3, doRay, rayLabel, rayMerge);
		Op(OP_LABEL, 1, rayLabel);
		{
			unsigned int offset, origin, scene, histVis, histLen, histSky, enough, rays, raysF, samplesF, step, skyStep;
			unsigned int fragXY, ignDot, ignFrac, ignScaled, ign, ignSamples, ignInt, sunStart;
			unsigned int ignShift, ignSky, ignSkyScaled, ignSkyInt, skyStart, nearlyY, upAxis, tCross, tangent, bitangent;
			unsigned int outerHeader, outerCond, outerBody, outerContinue, outerMerge;
			unsigned int index, go, index2;
			unsigned int offs, pointRaw, point, pointF, pointH, fracP, r, ang, cosA, sinA, rc, rs, du, dv, dirA, dirB, dir;
			unsigned int sOffs, sRaw, sPoint, sPointF, sPointH, sFrac, sR, sAng, sCos, sSin, sRc, sRs, sUp1, sUp, sT, sB, sN, sDirA, skyDir;
			unsigned int hitsEnd, hitsEndF, blockedFrac, raw, skyHitsEnd, skyHitsF, skyBlocked, skyRaw;
			unsigned int lenA, lenNew, invLen, single, sunWeight, delta, deltaScaled, visNew, skyDelta, skyDeltaScaled, skyNew;

			/* Start the rays slightly off the surface, to avoid them hitting the face they started on */
			offset = Id(); origin = Id(); scene = Id();
			Op(OP_VECTOR_TIMES_SCALAR, 4, tVec3, offset, n, cOffset);
			Op(OP_FADD, 4, tVec3, origin, world, offset);
			Op(OP_LOAD, 3, tScene, scene, vScene);

			trace.tBool = tBool; trace.tInt = tInt; trace.tUint = tUint; trace.tFloat = tFloat;
			trace.tVec2 = tVec2; trace.tVec4 = tVec4; trace.tSampled = tSampled; trace.tUVPtr = tUVPtr;
			trace.tTablePtr = tTablePtr; trace.pParamsTable = pParamsTable; trace.vParams = vParams; trace.vTex = vTex;
			trace.rayQuery = rayQuery; trace.scene = scene; trace.cFlags = cFlags; trace.cMask = cMask;
			trace.cNone = cNone; trace.cCommitted = cCommitted; trace.cZero = cZero; trace.cOne = cOne; trace.cHalf = cHalf;
			for (i = 0; i < 4; i++) trace.cIdx[i] = cIdx[i];

			/* Only 1 ray of each type per frame is needed once there's enough history */
			/*  (4 otherwise, so that surfaces that just became visible aren't noisy) */
			histVis = Id(); histLen = Id(); histSky = Id(); enough = Id(); rays = Id(); raysF = Id();
			samplesF = Id(); step = Id(); skyStep = Id();
			Op(OP_LOAD, 3, tFloat, histVis, vHistVis);
			Op(OP_LOAD, 3, tFloat, histLen, vHistLen);
			Op(OP_LOAD, 3, tFloat, histSky, vHistSky);
			Op(OP_FORD_GREATER_EQUAL, 4, tBool, enough, histLen, cFour);
			Op(OP_SELECT, 5, tInt, rays, enough, cInt1, cIdx[4]);
			Op(OP_CONVERT_S_TO_F, 3, tFloat, raysF,    rays);
			Op(OP_CONVERT_S_TO_F, 3, tFloat, samplesF, samples);
			Op(OP_SDIV, 4, tInt, step,    samples,    rays);
			Op(OP_SDIV, 4, tInt, skyStep, cSkyPoints, rays);

			/* The sun is a disc rather than a point, so each frame a different point on the disc is used, */
			/*  and the results are blended over multiple frames to give soft shadows. Neighbouring pixels */
			/*  start at different points (interleaved gradient noise), so that the noise is very fine */
			fragXY = Id(); ignDot = Id(); ignFrac = Id(); ignScaled = Id(); ign = Id(); ignSamples = Id(); ignInt = Id(); sunStart = Id();
			Op(OP_VECTOR_SHUFFLE, 6, tVec2, fragXY, fragPosV, fragPosV, 0, 1);
			Op(OP_DOT,  4, tFloat, ignDot, fragXY, cIgnDir);
			Op(OP_EXT_INST, 5, tFloat, ignFrac, ext, GLSL_FRACT, ignDot);
			Op(OP_FMUL, 4, tFloat, ignScaled, ignFrac, cIgnScale);
			Op(OP_EXT_INST, 5, tFloat, ign, ext, GLSL_FRACT, ignScaled);
			Op(OP_FMUL, 4, tFloat, ignSamples, ign, samplesF);
			Op(OP_CONVERT_F_TO_S, 3, tInt, ignInt, ignSamples);
			Op(OP_IADD, 4, tInt, sunStart, frameNum, ignInt);

			/* Sky rays use a different starting point, so they aren't in the same direction as the sun rays */
			ignShift = Id(); ignSky = Id(); ignSkyScaled = Id(); ignSkyInt = Id(); skyStart = Id();
			Op(OP_FADD, 4, tFloat, ignShift, ign, cGoldenFrac);
			Op(OP_EXT_INST, 5, tFloat, ignSky, ext, GLSL_FRACT, ignShift);
			Op(OP_FMUL, 4, tFloat, ignSkyScaled, ignSky, cSkyPointsF);
			Op(OP_CONVERT_F_TO_S, 3, tInt, ignSkyInt, ignSkyScaled);
			Op(OP_IADD, 4, tInt, skyStart, frameNum, ignSkyInt);

			/* Two directions along the face, for sky rays in the hemisphere above the face */
			nearlyY = Id(); upAxis = Id(); tCross = Id(); tangent = Id(); bitangent = Id();
			Op(OP_FORD_GREATER_THAN, 4, tBool, nearlyY, nyAbs, cAxis);
			Op(OP_SELECT,   5, tVec3, upAxis, nearlyY, cVec3X, cVec3Y);
			Op(OP_EXT_INST, 6, tVec3, tCross, ext, GLSL_CROSS, upAxis, n);
			Op(OP_EXT_INST, 5, tVec3, tangent, ext, GLSL_NORMALIZE, tCross);
			Op(OP_EXT_INST, 6, tVec3, bitangent, ext, GLSL_CROSS, n, tangent);

			Op(OP_STORE, 2, vIndex,   cInt0);
			Op(OP_STORE, 2, vHits,    cInt0);
			Op(OP_STORE, 2, vSkyHits, cInt0);
			outerHeader = Id(); outerCond = Id(); outerBody = Id(); outerContinue = Id(); outerMerge = Id();
			Op(OP_BRANCH, 1, outerHeader);
			Op(OP_LABEL,  1, outerHeader);
			Op(OP_LOOP_MERGE, 3, outerMerge, outerContinue, 0);
			Op(OP_BRANCH, 1, outerCond);
			Op(OP_LABEL,  1, outerCond);

			index = Id(); go = Id();
			Op(OP_LOAD, 3, tInt, index, vIndex);
			Op(OP_SLESS_THAN, 4, tBool, go, index, rays);
			Op(OP_BRANCH_CONDITIONAL, 3, go, outerBody, outerMerge);
			Op(OP_LABEL, 1, outerBody);

			/* Points on the sun's disc are spread out using a Vogel (golden angle) spiral */
			offs = Id(); pointRaw = Id(); point = Id(); pointF = Id(); pointH = Id(); fracP = Id(); r = Id(); ang = Id();
			cosA = Id(); sinA = Id(); rc = Id(); rs = Id(); du = Id(); dv = Id(); dirA = Id(); dirB = Id(); dir = Id();
			Op(OP_IMUL, 4, tInt, offs,     index, step);
			Op(OP_IADD, 4, tInt, pointRaw, sunStart, offs);
			Op(OP_SMOD, 4, tInt, point,    pointRaw, samples);
			Op(OP_CONVERT_S_TO_F, 3, tFloat, pointF, point);
			Op(OP_FADD, 4, tFloat, pointH, pointF, cHalf);
			Op(OP_FDIV, 4, tFloat, fracP,  pointH, samplesF);
			Op(OP_EXT_INST, 5, tFloat, r, ext, GLSL_SQRT, fracP);
			Op(OP_FMUL, 4, tFloat, ang, pointF, cGolden);
			Op(OP_EXT_INST, 5, tFloat, cosA, ext, GLSL_COS, ang);
			Op(OP_EXT_INST, 5, tFloat, sinA, ext, GLSL_SIN, ang);
			Op(OP_FMUL, 4, tFloat, rc, r, cosA);
			Op(OP_FMUL, 4, tFloat, rs, r, sinA);
			Op(OP_VECTOR_TIMES_SCALAR, 4, tVec3, du, sunU, rc);
			Op(OP_VECTOR_TIMES_SCALAR, 4, tVec3, dv, sunV, rs);
			Op(OP_FADD, 4, tVec3, dirA, sun,  du);
			Op(OP_FADD, 4, tVec3, dirB, dirA, dv);
			Op(OP_EXT_INST, 5, tVec3, dir, ext, GLSL_NORMALIZE, dirB);
			EmitTrace(&trace, doSun, origin, dir, dist, vHits);

			/* Sky rays use the same spiral, lifted up onto the hemisphere above the face */
			/*  (which gives directions weighted by how much light they contribute to the face) */
			sOffs = Id(); sRaw = Id(); sPoint = Id(); sPointF = Id(); sPointH = Id(); sFrac = Id(); sR = Id(); sAng = Id();
			sCos = Id(); sSin = Id(); sRc = Id(); sRs = Id(); sUp1 = Id(); sUp = Id(); sT = Id(); sB = Id(); sN = Id();
			sDirA = Id(); skyDir = Id();
			Op(OP_IMUL, 4, tInt, sOffs,  index, skyStep);
			Op(OP_IADD, 4, tInt, sRaw,   skyStart, sOffs);
			Op(OP_SMOD, 4, tInt, sPoint, sRaw, cSkyPoints);
			Op(OP_CONVERT_S_TO_F, 3, tFloat, sPointF, sPoint);
			Op(OP_FADD, 4, tFloat, sPointH, sPointF, cHalf);
			Op(OP_FDIV, 4, tFloat, sFrac,   sPointH, cSkyPointsF);
			Op(OP_EXT_INST, 5, tFloat, sR, ext, GLSL_SQRT, sFrac);
			Op(OP_FMUL, 4, tFloat, sAng, sPointF, cGolden);
			Op(OP_EXT_INST, 5, tFloat, sCos, ext, GLSL_COS, sAng);
			Op(OP_EXT_INST, 5, tFloat, sSin, ext, GLSL_SIN, sAng);
			Op(OP_FMUL, 4, tFloat, sRc, sR, sCos);
			Op(OP_FMUL, 4, tFloat, sRs, sR, sSin);
			Op(OP_FSUB, 4, tFloat, sUp1, cOne, sFrac);
			Op(OP_EXT_INST, 5, tFloat, sUp, ext, GLSL_SQRT, sUp1);
			Op(OP_VECTOR_TIMES_SCALAR, 4, tVec3, sT, tangent,   sRc);
			Op(OP_VECTOR_TIMES_SCALAR, 4, tVec3, sB, bitangent, sRs);
			Op(OP_VECTOR_TIMES_SCALAR, 4, tVec3, sN, n,         sUp);
			Op(OP_FADD, 4, tVec3, sDirA,  sT,    sB);
			Op(OP_FADD, 4, tVec3, skyDir, sDirA, sN);
			EmitTrace(&trace, doSky, origin, skyDir, aoRange, vSkyHits);

			Op(OP_BRANCH, 1, outerContinue);
			Op(OP_LABEL,  1, outerContinue);
			index2 = Id();
			Op(OP_IADD,   4, tInt, index2, index, cInt1);
			Op(OP_STORE,  2, vIndex, index2);
			Op(OP_BRANCH, 1, outerHeader);
			Op(OP_LABEL,  1, outerMerge);

			/* Blend this frame's results into the history (equally weighted until maxHistory frames) */
			/*  When the sun is just a point (samples < 4), every frame gives the same exact result anyway */
			hitsEnd = Id(); hitsEndF = Id(); blockedFrac = Id(); raw = Id();
			skyHitsEnd = Id(); skyHitsF = Id(); skyBlocked = Id(); skyRaw = Id();
			lenA = Id(); lenNew = Id(); invLen = Id(); single = Id(); sunWeight = Id();
			delta = Id(); deltaScaled = Id(); visNew = Id(); skyDelta = Id(); skyDeltaScaled = Id(); skyNew = Id();
			Op(OP_LOAD, 3, tInt, hitsEnd,    vHits);
			Op(OP_LOAD, 3, tInt, skyHitsEnd, vSkyHits);
			Op(OP_CONVERT_S_TO_F, 3, tFloat, hitsEndF, hitsEnd);
			Op(OP_CONVERT_S_TO_F, 3, tFloat, skyHitsF, skyHitsEnd);
			Op(OP_FDIV, 4, tFloat, blockedFrac, hitsEndF, raysF);
			Op(OP_FDIV, 4, tFloat, skyBlocked,  skyHitsF, raysF);
			Op(OP_FSUB, 4, tFloat, raw,    cOne, blockedFrac);
			Op(OP_FSUB, 4, tFloat, skyRaw, cOne, skyBlocked);
			Op(OP_FADD, 4, tFloat, lenA, histLen, cOne);
			Op(OP_EXT_INST, 6, tFloat, lenNew, ext, GLSL_FMIN, lenA, maxHistory);
			Op(OP_FDIV, 4, tFloat, invLen, cOne, lenNew);
			Op(OP_SLESS_THAN, 4, tBool, single, samples, cIdx[4]);
			Op(OP_SELECT, 5, tFloat, sunWeight, single, cOne, invLen);
			Op(OP_FSUB, 4, tFloat, delta, raw, histVis);
			Op(OP_FMUL, 4, tFloat, deltaScaled, delta, sunWeight);
			Op(OP_FADD, 4, tFloat, visNew, histVis, deltaScaled);
			Op(OP_FSUB, 4, tFloat, skyDelta, skyRaw, histSky);
			Op(OP_FMUL, 4, tFloat, skyDeltaScaled, skyDelta, invLen);
			Op(OP_FADD, 4, tFloat, skyNew, histSky, skyDeltaScaled);
			Op(OP_STORE,  2, vVisible, visNew);
			Op(OP_STORE,  2, vLen,     lenNew);
			Op(OP_STORE,  2, vSky,     skyNew);
			Op(OP_BRANCH, 1, rayMerge);
		}
		Op(OP_LABEL, 1, rayMerge);

		visible = Id(); newLen = Id(); sky = Id(); history = Id(); faceShifted = Id(); faceLen = Id();
		Op(OP_LOAD, 3, tFloat, visible, vVisible);
		Op(OP_LOAD, 3, tFloat, newLen,  vLen);
		Op(OP_LOAD, 3, tFloat, sky,     vSky);
		Op(OP_FMUL, 4, tFloat, faceShifted, faceID, cSkyPointsF);
		Op(OP_FADD, 4, tFloat, faceLen,     faceShifted, newLen);
		Op(OP_COMPOSITE_CONSTRUCT, 6, tVec4, history, visible, viewDepth, faceLen, sky);
		Op(OP_STORE, 2, vOutHistory, history);

		/* light = shadowCol * face * ambient + (sunCol - shadowCol) * direct * visible, but divided by */
		/*  the sunCol * face that the vertex colour already includes (and then per colour channel) */
		/* Ambient light is reduced by how much of the sky is blocked (ambient occlusion / sky light) */
		skyLost = Id(); skyScaled = Id(); ambient = Id(); ambientRatio = Id();
		oneMinus = Id(); lightK1 = Id(); lightK = Id(); lightAdd = Id(); light = Id();
		rgbCol = Id(); lit = Id(); litRgb = Id(); litCol = Id();
		Op(OP_FSUB, 4, tFloat, skyLost,   cOne, sky);
		Op(OP_FMUL, 4, tFloat, skyScaled, aoStrength, skyLost);
		Op(OP_FSUB, 4, tFloat, ambient,   cOne, skyScaled);
		Op(OP_VECTOR_TIMES_SCALAR, 4, tVec3, ambientRatio, ratio, ambient);
		Op(OP_FSUB, 4, tVec3,  oneMinus, cVec3One, ratio);
		Op(OP_FMUL, 4, tFloat, lightK1,  direct, visible);
		Op(OP_FDIV, 4, tFloat, lightK,   lightK1, face);
		Op(OP_VECTOR_TIMES_SCALAR, 4, tVec3, lightAdd, oneMinus, lightK);
		Op(OP_FADD, 4, tVec3, light, ambientRatio, lightAdd);
		Op(OP_SELECT, 5, tVec3, lit, shadedCol, light, cVec3One);

		Op(OP_VECTOR_SHUFFLE, 7, tVec3, rgbCol, col, col, 0, 1, 2);
		Op(OP_FMUL,           4, tVec3, litRgb, rgbCol, lit);
		Op(OP_VECTOR_SHUFFLE, 8, tVec4, litCol, litRgb, col, 0, 1, 2, 6);
		col = litCol;
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
