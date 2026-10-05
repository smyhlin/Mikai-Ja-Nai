/* Native status/control ABI v2. Plain C only; AJI ABI-8 is unchanged. */
#ifndef AJI_NATIVE_H
#define AJI_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(AJI_NATIVE_BUILD)
#    define AJI_NATIVE_EXPORT __declspec(dllexport)
#  else
#    define AJI_NATIVE_EXPORT __declspec(dllimport)
#  endif
#else
#  define AJI_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AJI_NATIVE_PROTOCOL_VERSION 2u
#define AJI_NATIVE_SCHEMA_VERSION 2u
#define AJI_NATIVE_TEXT_CAPACITY 128u
#define AJI_NATIVE_MAX_STAGES 9u
#define AJI_NATIVE_SHA256_CAPACITY 65u
#define AJI_NATIVE_BUILD_KEY_CAPACITY 128u

typedef struct aji_ctx aji_ctx;

typedef enum aji_native_result {
    AJI_NATIVE_OK = 0,
    AJI_NATIVE_NO_CONTEXT = 1,
    AJI_NATIVE_PROTOCOL_MISMATCH = 2,
    AJI_NATIVE_INVALID_ARGUMENT = 3,
    AJI_NATIVE_BACKEND_UNAVAILABLE = 4,
    AJI_NATIVE_OPERATION_FAILED = 5,
    AJI_NATIVE_UNSUPPORTED = 6
} aji_native_result;

typedef enum aji_native_state {
    AJI_NATIVE_STATE_UNAVAILABLE = 0,
    AJI_NATIVE_STATE_PROBE = 1,
    AJI_NATIVE_STATE_PREPARED = 2,
    AJI_NATIVE_STATE_ACTIVE = 3,
    AJI_NATIVE_STATE_DRAINING = 4,
    AJI_NATIVE_STATE_SUSPENDED = 5,
    AJI_NATIVE_STATE_FAILED = 6
} aji_native_state;

typedef enum aji_native_control_action {
    AJI_NATIVE_CONTROL_DRAIN = 1,
    AJI_NATIVE_CONTROL_SUSPEND = 2,
    AJI_NATIVE_CONTROL_RESUME = 3
} aji_native_control_action;

typedef enum aji_native_capability {
    AJI_NATIVE_CAP_PROBE_PASSTHROUGH = 1u << 0,
    AJI_NATIVE_CAP_CU_DEVICE_UUID = 1u << 1,
    AJI_NATIVE_CAP_D3D_ADAPTER_LUID = 1u << 2,
    AJI_NATIVE_CAP_PREPARED_ONLY = 1u << 3,
    AJI_NATIVE_CAP_TICKET_DRAIN = 1u << 4,
    AJI_NATIVE_CAP_SUSPEND_RESUME = 1u << 5
} aji_native_capability;

/* Snapshots are observations, not preparation attestations. A producer must
   leave effective fields empty and report Unavailable absent verified plan proof. */
typedef struct aji_native_snapshot_v2 {
    uint32_t struct_size; /* must exactly match sizeof(aji_native_snapshot_v2) */
    uint32_t requested_protocol_version;
    uint32_t protocol_version;
    uint32_t schema_version;
    uint32_t aji_abi_version;
    uint32_t result;
    uint32_t state;
    uint32_t capability_bits;
    uint32_t backend;
    uint64_t revision;
    uint64_t epoch;
    uint64_t last_submitted_ticket;
    uint64_t completed_ticket;
    uint32_t requested_slot;
    uint32_t effective_slot;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t stage_count;
    uint32_t reserved;
    char provider[AJI_NATIVE_TEXT_CAPACITY];
    char requested_plan[AJI_NATIVE_TEXT_CAPACITY];
    char effective_plan[AJI_NATIVE_TEXT_CAPACITY];
    char device_identity[AJI_NATIVE_TEXT_CAPACITY];
    char error[AJI_NATIVE_TEXT_CAPACITY];
    char engine_sha256[AJI_NATIVE_MAX_STAGES][AJI_NATIVE_SHA256_CAPACITY];
    char model_sha256[AJI_NATIVE_MAX_STAGES][AJI_NATIVE_SHA256_CAPACITY];
    char prepared_build_keys[AJI_NATIVE_MAX_STAGES][AJI_NATIVE_BUILD_KEY_CAPACITY]; /* empty unless independently verified */
} aji_native_snapshot_v2;

AJI_NATIVE_EXPORT int aji_native_get_snapshot(
    aji_ctx *ctx, uint32_t requested_protocol, aji_native_snapshot_v2 *snapshot);

/* ------------------------------------------------------------------------- *
 * Prepared plans (protocol v1). Additive: AJI ABI-8, the v2 snapshot layout
 * and the existing status/control exports are unchanged. A prepared plan is
 * the caller's complete, ordered description of one admitted chain: the
 * source, device and build facts it was resolved against, and the exact
 * stages to load, each naming the digests, owned paths, binding names and
 * geometry its identity is checked against. A producer that does not
 * implement the prepared exports exports no backend prepared symbol; the
 * dispatcher then refuses by name rather than guessing.
 * ------------------------------------------------------------------------- */

#define AJI_NATIVE_PREPARED_PROTOCOL_VERSION 1u
#define AJI_NATIVE_PREPARED_SCHEMA_VERSION 1u

#define AJI_NATIVE_BINDING_NAME_CAPACITY 64u
#define AJI_NATIVE_PATH_CAPACITY 260u
#define AJI_NATIVE_CONTENT_ID_CAPACITY 128u
#define AJI_NATIVE_BUILD_OPTIONS_CAPACITY 512u

#define AJI_NATIVE_PLAN_FLAG_PREPARED_ONLY 1u

typedef enum aji_native_stage_role {
    AJI_NATIVE_ROLE_SPATIAL = 1,
    AJI_NATIVE_ROLE_TEMPORAL = 2
} aji_native_stage_role;

typedef enum aji_native_stage_order {
    AJI_NATIVE_ORDER_NONE = 0,           /* spatial stages */
    AJI_NATIVE_ORDER_BEFORE_SPATIAL = 1, /* temporal: interpolate first */
    AJI_NATIVE_ORDER_AFTER_SPATIAL = 2   /* temporal: interpolate last */
} aji_native_stage_order;

typedef enum aji_native_element_type {
    AJI_NATIVE_DTYPE_FP16 = 1,
    AJI_NATIVE_DTYPE_FP32 = 2
} aji_native_element_type;

typedef enum aji_native_tensor_layout {
    AJI_NATIVE_LAYOUT_CHW = 1
} aji_native_tensor_layout;

/* One ordered stage. Every text field is NUL-terminated within its capacity
   and every numeric field is validated, refused by name rather than
   defaulted. input/output_width/height are the geometry the engine is bound
   at; content_width/height is the frame window carried through it (equal for
   a spatial stage, the unpadded frame for a temporal one). */
typedef struct aji_native_prepared_stage_v1 {
    uint32_t struct_size;      /* must equal sizeof(aji_native_prepared_stage_v1) */
    uint32_t role;             /* aji_native_stage_role */
    uint32_t order;            /* aji_native_stage_order; spatial = ORDER_NONE */
    uint32_t ordinal;          /* 0-based execution position, strictly ascending */
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t content_width;
    uint32_t content_height;
    uint32_t pad_left;         /* temporal centring pad; 0 for spatial */
    uint32_t pad_top;
    uint32_t input_channels;
    uint32_t output_channels;
    uint32_t input_dtype;      /* aji_native_element_type */
    uint32_t output_dtype;
    uint32_t input_layout;     /* aji_native_tensor_layout */
    uint32_t output_layout;
    uint32_t factor_num;       /* temporal interpolation ratio; 0/0 spatial */
    uint32_t factor_den;
    double scene_threshold;    /* temporal scene-cut threshold, [0,1] */
    char model_id[AJI_NATIVE_TEXT_CAPACITY];
    char model_content_id[AJI_NATIVE_CONTENT_ID_CAPACITY];
    char model_sha256[AJI_NATIVE_SHA256_CAPACITY];
    char engine_sha256[AJI_NATIVE_SHA256_CAPACITY];
    char build_key[AJI_NATIVE_BUILD_KEY_CAPACITY];
    char model_path[AJI_NATIVE_PATH_CAPACITY];
    char engine_path[AJI_NATIVE_PATH_CAPACITY];
    char input_tensor[AJI_NATIVE_BINDING_NAME_CAPACITY];  /* empty = discover */
    char output_tensor[AJI_NATIVE_BINDING_NAME_CAPACITY];
} aji_native_prepared_stage_v1;

/* The whole admitted plan. stages[] is inline and fixed-capacity; stage_count
   is the number of populated entries, in ordinal order. */
typedef struct aji_native_prepared_plan_v1 {
    uint32_t struct_size;       /* must equal sizeof(aji_native_prepared_plan_v1) */
    uint32_t requested_protocol;/* AJI_NATIVE_PREPARED_PROTOCOL_VERSION */
    uint32_t stage_count;       /* 1..AJI_NATIVE_MAX_STAGES */
    uint32_t flags;             /* AJI_NATIVE_PLAN_FLAG_* */
    uint64_t revision;          /* caller plan revision, echoed for ack correlation */
    uint64_t epoch;             /* caller source epoch */
    char requested_plan[AJI_NATIVE_TEXT_CAPACITY];
    char recipe_id[AJI_NATIVE_TEXT_CAPACITY];
    char backend[AJI_NATIVE_TEXT_CAPACITY];

    char device_identity[AJI_NATIVE_TEXT_CAPACITY];

    /* source geometry: existing SourceGeometry facts, passed through unchanged */
    uint32_t source_coded_width;
    uint32_t source_coded_height;
    uint32_t source_crop_x;
    uint32_t source_crop_y;
    uint32_t source_crop_width;
    uint32_t source_crop_height;
    uint32_t source_rotation;   /* 0/90/180/270 */
    uint32_t source_fps_num;
    uint32_t source_fps_den;
    uint32_t source_bit_depth;  /* 8/10/12/...; 0 unknown */
    char source_csp[AJI_NATIVE_TEXT_CAPACITY];
    char source_matrix[AJI_NATIVE_TEXT_CAPACITY];
    char source_range[AJI_NATIVE_TEXT_CAPACITY];
    char source_siting[AJI_NATIVE_TEXT_CAPACITY];

    /* device facts: existing DeviceIdentity facts, passed through unchanged */
    char decoder_device_uuid[AJI_NATIVE_TEXT_CAPACITY];
    char compute_device_uuid[AJI_NATIVE_TEXT_CAPACITY];
    char adapter_luid[AJI_NATIVE_TEXT_CAPACITY];
    uint32_t device_sm_major;
    uint32_t device_sm_minor;

    /* build facts: existing EngineBuildIdentity facts, passed through unchanged */
    char build_provider[AJI_NATIVE_TEXT_CAPACITY];
    char build_runtime[AJI_NATIVE_TEXT_CAPACITY];
    char build_plugin[AJI_NATIVE_TEXT_CAPACITY];
    char build_core_sha[AJI_NATIVE_TEXT_CAPACITY];
    uint32_t build_source_abi;
    char build_precision[AJI_NATIVE_TEXT_CAPACITY];
    uint32_t build_tf32;
    uint32_t build_strongly_typed;
    uint32_t build_skip_inference;
    char build_options[AJI_NATIVE_BUILD_OPTIONS_CAPACITY];

    aji_native_prepared_stage_v1 stages[AJI_NATIVE_MAX_STAGES];
} aji_native_prepared_plan_v1;

/* The create-parameter type is declared in <aji.h>; forward-declared here so
   this header stays usable on its own for the prepared exports. */
struct aji_create_params;

/* Create a context from caller-supplied parameters and admit the plan in one
   step: no conf file, no engine-filename/cache inference and no build path is
   consulted. Returns the new context, or NULL with *snapshot holding the
   named refusal. */
AJI_NATIVE_EXPORT aji_ctx *aji_native_create_prepared(
    const struct aji_create_params *params,
    const aji_native_prepared_plan_v1 *plan,
    aji_native_snapshot_v2 *snapshot);

/* Admit (or transactionally replace) a prepared plan on an existing context.
   Validation precedes any deserialize or device work; on refusal the previous
   effective plan is retained and *snapshot names the reason. */
AJI_NATIVE_EXPORT int aji_native_apply_prepared(
    aji_ctx *ctx, const aji_native_prepared_plan_v1 *plan,
    aji_native_snapshot_v2 *snapshot);

/* Lifecycle control that orders the drain against the caller's inference
   stream (a NULL stream means the default stream). Same actions as
   aji_native_control. */
AJI_NATIVE_EXPORT int aji_native_control_on_stream(
    aji_ctx *ctx, uint32_t requested_protocol, uint32_t action,
    uint64_t epoch, uint32_t timeout_ms, void *stream,
    aji_native_snapshot_v2 *snapshot);
AJI_NATIVE_EXPORT int aji_native_control(
    aji_ctx *ctx, uint32_t requested_protocol, uint32_t action,
    uint64_t epoch, uint32_t timeout_ms, aji_native_snapshot_v2 *snapshot);

/* ------------------------------------------------------------------------- *
 * Factory capability query (query protocol v1). Additive: AJI ABI-8, the v2
 * snapshot layout, the prepared-plan v1 ABI and every existing export are
 * unchanged. A caller that already holds the actually adopted decoder CUDA
 * context asks the producer what it can really be asked to prepare; the
 * producer answers by running its real backend factory against that borrowed
 * context and never from the presence of an export, a preference, a filename
 * or a manifest. No context, model, engine or network is created here and no
 * build/cache path is consulted.
 * ------------------------------------------------------------------------- */

#define AJI_NATIVE_FACTORY_QUERY_PROTOCOL_VERSION 1u
#define AJI_NATIVE_FACTORY_REASON_KEY_CAPACITY 64u
#define AJI_NATIVE_FACTORY_REASON_CAPACITY 192u

typedef enum aji_native_factory_backend {
    AJI_NATIVE_FACTORY_BACKEND_NONE = 0,
    AJI_NATIVE_FACTORY_BACKEND_TENSORRT = 1,
    AJI_NATIVE_FACTORY_BACKEND_DIRECTML = 2
} aji_native_factory_backend;

/* Roles the queried backend can actually be asked to admit as prepared-plan
   v1 stages. Bit 0 is the generic fp16 spatial upscale/reconstruct stage;
   bit 1 is the original 11-channel RIFE temporal stage. */
typedef enum aji_native_factory_role {
    AJI_NATIVE_FACTORY_ROLE_SPATIAL = 1u << 0,
    AJI_NATIVE_FACTORY_ROLE_RIFE = 1u << 1
} aji_native_factory_role;

/* One observation of the actually adopted source. result is AJI_NATIVE_OK only
   when the answered factory is genuinely usable for the adopted device, and
   only then do backend, role_bits, device_identity and runtime_version carry
   values; every other outcome names itself in reason_key/reason and never
   reports a capability derived from export presence, a preference, a filename
   or a manifest. device_identity is the actual CUDA UUID as 32 lower-case hex,
   never a pointer address or a device ordinal. */
typedef struct aji_native_factory_capabilities_v1 {
    uint32_t struct_size;        /* must equal sizeof(aji_native_factory_capabilities_v1) */
    uint32_t requested_protocol; /* AJI_NATIVE_FACTORY_QUERY_PROTOCOL_VERSION */
    uint32_t protocol_version;   /* AJI_NATIVE_FACTORY_QUERY_PROTOCOL_VERSION */
    uint32_t result;             /* aji_native_result */
    uint32_t native_protocol_version; /* AJI_NATIVE_PROTOCOL_VERSION */
    uint32_t native_schema_version;   /* AJI_NATIVE_SCHEMA_VERSION */
    uint32_t source_abi;              /* AJI ABI version */
    uint32_t backend;                 /* aji_native_factory_backend */
    uint32_t role_bits;               /* aji_native_factory_role */
    uint32_t sm_major;
    uint32_t sm_minor;
    char device_identity[AJI_NATIVE_TEXT_CAPACITY];
    char provider[AJI_NATIVE_TEXT_CAPACITY];
    char runtime_version[AJI_NATIVE_TEXT_CAPACITY];
    char reason_key[AJI_NATIVE_FACTORY_REASON_KEY_CAPACITY];
    char reason[AJI_NATIVE_FACTORY_REASON_CAPACITY];
} aji_native_factory_capabilities_v1;

/* Ask the producer what it can actually prepare for the source in params: a
   borrowed, already-current CUDA context/device it must adopt and never
   create. The backend export is optional: a producer that does not implement
   it is refused by name while the legacy ABI-8, v2 snapshot/control and
   prepared-plan calls keep working. */
AJI_NATIVE_EXPORT int aji_native_query_factory(
    const struct aji_create_params *params,
    aji_native_factory_capabilities_v1 *out);

#ifdef __cplusplus
}
#endif
#endif
