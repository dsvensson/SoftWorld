// qc_config.c -- VM configurations and their defaults

#include "qc_local.h"

// fields zeroed when an entity is removed: FTE's CSQC frees these; its SSQC
// clears id's fields in the engine
static const char *const csqc_clears[] = {"solid", "movetype", "modelindex", "think", "nextthink",
	"predraw", "drawmask", "renderflags", NULL};
static const char *const ssqc_clears[] = {"model", "modelindex", "solid", "classname", NULL};

// spawned CSQC entities collide with every dimension
static const qc_spawndefault_t	csqc_spawn[] = {{"dimension_solid", "dimension_default", 255},
	{"dimension_hit", "dimension_default", 255}};

static const char *const shared_default[] = {"self", "other", "time", "frametime", NULL};

void QC_DefaultConfig (qc_config_t *config, qc_kind_t kind)
{
	*config = (qc_config_t){
		.kind = kind,
		.limits = {
			.max_edicts = 65536,
			.local_stack_words = 1u << 20,
			.call_depth = 1024,
			.runaway = 100000000,
			.deadline = 0,
			.reentry = 64,
			.heap_bytes = 64u << 20,
			.temp_strings = 1u << 20,
			.temp_string_bytes = (size_t)256 << 20,
			.progs_area_bytes = 16u << 20,
			.progs = 16,
			.threads = 1024,
			.thread_bytes = (size_t)16 << 20,
			.string_buffers = 1024,
			.string_buffer_entries = 1u << 20,
			.hash_tables = 1024,
			.container_bytes = (size_t)64 << 20,
			.files = 256,
			.warnings_per_call = 64,
		},
		.charscheme = QC_CHARS_QUAKE,
		.seed = 0x5EED0FC5C0DEull,
		.state_step = 0.1f,
		.field_reserve_bytes = 256,
		.shared_globals = shared_default,
	};
	switch (kind)
	{
	case QC_CSQC:
		config->remove_clears = csqc_clears;
		config->spawn_defaults = csqc_spawn;
		config->num_spawn_defaults = 2;
		break;
	case QC_SSQC:
		config->remove_clears = ssqc_clears;
		break;
	case QC_MENU:
		break;
	}
}

qc_value_t QC_ValFloat (float f)
{
	return (qc_value_t){{QC_FloatBits (f), 0, 0}};
}

qc_value_t QC_ValInt (int32_t i)
{
	return (qc_value_t){{(uint32_t)i, 0, 0}};
}

qc_value_t QC_ValVector (float x, float y, float z)
{
	return (qc_value_t){{QC_FloatBits (x), QC_FloatBits (y), QC_FloatBits (z)}};
}

qc_value_t QC_ValWord (uint32_t u)
{
	return (qc_value_t){{u, 0, 0}};
}
