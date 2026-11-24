/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "sysemu/hostmem.h"
#include "pager.h"
#include "migration/misc.h"
#include "qapi/qapi-commands-misc.h"
#include "qemu/accel.h"
#include "sysemu/runstate.h"
#include "sysemu/cpus.h"
#include "qapi/visitor.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"

typedef struct TensorRam {
    HostMemoryBackend parent;
    struct store *store;
    struct pager *pager;
    char *image;
    bool quiescent_reclaim;
    uint64_t resident_target;
    uint32_t search_nodes;
    bool adaptive;
    QEMUTimer *trim_timer;
} TensorRam;
static char *get_image(Object *obj, Error **errp)
{
    return g_strdup(((TensorRam *)obj)->image);
}
static void set_image(Object *obj, const char *value, Error **errp)
{
    TensorRam *r = (TensorRam *)obj;
    if (host_memory_backend_mr_inited(&r->parent)) {
        error_setg(errp, "initial image is fixed after allocation");
        return;
    }
    g_free(r->image);
    r->image = g_strdup(value);
}
static bool get_quiescent_reclaim(Object *obj, Error **errp)
{
    return ((TensorRam *)obj)->quiescent_reclaim;
}
static void set_quiescent_reclaim(Object *obj, bool value, Error **errp)
{
    TensorRam *r = (TensorRam *)obj;
    if (host_memory_backend_mr_inited(&r->parent)) {
        error_setg(errp, "reclaim mode is fixed after allocation");
        return;
    }
    r->quiescent_reclaim = value;
}
static bool reclaim_permitted(TensorRam *r, bool quiescent)
{
    Object *accel = OBJECT(current_accel());
    return vram_permitted(r->quiescent_reclaim, quiescent,
                          object_dynamic_cast(accel, ACCEL_CLASS_NAME("tcg")) != NULL,
                          object_dynamic_cast(accel, ACCEL_CLASS_NAME("kvm")) != NULL,
                          !pager_tracks_writes(r->pager), !migration_is_running());
}
VramReclaimInfo *qmp_x_vram_reclaim(const char *id, uint64_t offset,
                                     uint64_t length, Error **errp)
{
    Object *obj = object_resolve_path_component(object_get_objects_root(), id);
    TensorRam *r = (TensorRam *)object_dynamic_cast(obj, "memory-backend-verdant-tensor");
    struct reclaim_result result;

    if (!r || !r->pager || !r->quiescent_reclaim) {
        error_setg(errp, "expected a vRAM backend with x-quiescent-reclaim enabled");
        return NULL;
    }
    if (!reclaim_permitted(r, runstate_check(RUN_STATE_PAUSED))) {
        error_setg(errp, "quiescent reclaim requires paused TCG or capture-all KVM and inactive migration");
        return NULL;
    }
    if (pager_checkpoint_range(r->pager, offset, length, &result)) {
        error_setg_errno(errp, errno, "range reclaim failed after %" PRIu64 " pages",
                         result.discarded);
        return NULL;
    }
    VramReclaimInfo *info = g_new0(VramReclaimInfo, 1);
    info->discarded_pages = result.discarded;
    info->resident_after = result.resident_after;
    info->total_faults = pager_faults(r->pager);
    return info;
}
VramStoreStats *qmp_x_query_vram_store(const char *id, Error **errp)
{
    Object *obj = object_resolve_path_component(object_get_objects_root(), id);
    TensorRam *r = (TensorRam *)object_dynamic_cast(obj, "memory-backend-verdant-tensor");
    uint64_t counts[5];
    if (!r || !r->pager || !runstate_check(RUN_STATE_PAUSED)) {
        error_setg(errp, "store telemetry requires a paused vRAM backend");
        return NULL;
    }
    VramStoreStats *info = g_new0(VramStoreStats, 1);
    store_stats(r->store, counts);
    store_selector_stats(r->store, &info->selector_pages, &info->selector_restores);
    struct pager_metrics metrics;
    pager_metrics(r->pager, &metrics);
    store_edits(r->store, &info->corrected_pages, &info->edit_count);
    store_search_stats(r->store,&info->program_pages,&info->program_bytes,&info->searches,&info->search_nodes);
    store_feedback_stats(r->store,&info->plan_refits,&info->search_skips);
    info->resident_pages = metrics.resident;
    info->clean_drops = metrics.clean_drops;
    info->captures = metrics.captures;
    info->protected_writes = metrics.writes;
    info->automatic_drops = metrics.automatic_drops;
    info->write_tracking = metrics.write_tracking;
    info->raw_pages = counts[2];
    info->backing_bytes = counts[4];
    return info;
}
static bool load_image(TensorRam *r, Error **errp)
{
    if (!r->image) {
        return true;
    }
    FILE *f = fopen(r->image, "rb");
    if (!f) {
        error_setg_errno(errp, errno, "open initial tensor RAM image");
        return false;
    }
    uint64_t page_idx = 0;
    bool ok = true;
    for (;;) {
        uint8_t bytes[4096] = { 0 };
        size_t n = fread(bytes, 1, sizeof(bytes), f);
        if (ferror(f)) {
            error_setg_errno(errp, errno, "read initial image");
            ok = false;
            break;
        }
        if (!n) {
            break;
        }
        if (page_idx >= r->parent.size / sizeof(bytes) ||
            store_put(r->store, page_idx++, bytes)) {
            error_setg(errp, "initial page outside configured guest RAM");
            ok = false;
            break;
        }
    }
    fclose(f);
    return ok;
}
static void get_target(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = ((TensorRam *)obj)->resident_target;
    visit_type_size(v, name, &value, errp);
}
static void set_target(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    TensorRam *r = (TensorRam *)obj;
    if (host_memory_backend_mr_inited(&r->parent)) {
        error_setg(errp, "resident target is fixed after allocation");
        return;
    }
    visit_type_size(v, name, &r->resident_target, errp);
}
static void automatic_trim(void *opaque)
{
    TensorRam *r = opaque;
    uint64_t pages = r->resident_target / 4096;
    if (runstate_is_running() && !migration_is_running() && pager_over_target(r->pager, pages)) {
        pause_all_vcpus();
        if (!reclaim_permitted(r, true)) {
            error_report("vRAM reclaim requires exclusive TCG or capture-all KVM");
            vm_stop(RUN_STATE_INTERNAL_ERROR);
            return;
        }
        if (pager_trim(r->pager, pages) < 0) {
            error_report("vRAM reclaim failed: %s", strerror(errno));
            vm_stop(RUN_STATE_INTERNAL_ERROR);
            return;
        }
        resume_all_vcpus();
    }
    timer_mod(r->trim_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 10);
}
static bool tensor_alloc(HostMemoryBackend *b, Error **errp)
{
    TensorRam *r = (TensorRam *)b;
    g_autofree char *name = host_memory_backend_get_name(b);
    if (!b->size || b->size % 4096 || b->share || b->guest_memfd || b->prealloc) {
        error_setg(errp, "tensor backend requires private, demand-paged, 4 KiB aligned RAM");
        return false;
    }
    if (r->resident_target &&
        (!r->quiescent_reclaim || !vram_range(0, r->resident_target, b->size))) {
        error_setg(errp, "resident target requires an aligned in-bounds size and exclusive quiescent reclaim");
        return false;
    }
    if (r->search_nodes > 49) {
        error_setg(errp, "x-search-nodes must be in 0..49");
        return false;
    }
    r->store = store_new_config(b->size / 4096, r->search_nodes, r->adaptive);
    if (!r->store) {
        error_setg(errp, "tensor page directory allocation failed");
        return false;
    }
    if (!load_image(r, errp)) {
        return false;
    }
    if (!memory_region_init_ram_flags_nomigrate(&b->mr, OBJECT(b), name, b->size,
                    b->reserve ? 0 : RAM_NORESERVE, errp)) {
        return false;
    }
    /* KVM may cache writable secondary mappings. Capture every reclaimed frame
     * and rely on Linux MMU invalidation during MADV_DONTNEED while CPUs are stopped. */
    bool track = object_dynamic_cast(OBJECT(current_accel()), ACCEL_CLASS_NAME("tcg")) != NULL;
    r->pager = pager_attach_vm(memory_region_get_ram_ptr(&b->mr), b->size, r->store,
                               track, r->resident_target != 0);
    if (!r->pager) {
        error_setg_errno(errp, errno, "attaching Verdant missing-page handler");
        return false;
    }
    if (r->resident_target) {
        r->trim_timer = timer_new_ms(QEMU_CLOCK_REALTIME, automatic_trim, r);
        timer_mod(r->trim_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 10);
    }
    return true;
}
static void tensor_finalize(Object *obj)
{
    TensorRam *r = (TensorRam *)obj;
    if (r->trim_timer) {
        timer_free(r->trim_timer);
    }
    pager_detach(r->pager);
    g_free(r->image);
    if (r->store) {
        store_free(r->store);
    }
}
static void get_search_nodes(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint32_t value = ((TensorRam *)obj)->search_nodes;
    visit_type_uint32(v, name, &value, errp);
}
static void set_search_nodes(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    TensorRam *r = (TensorRam *)obj;
    if (host_memory_backend_mr_inited(&r->parent)) {
        error_setg(errp, "search budget is fixed after allocation");
        return;
    }
    visit_type_uint32(v, name, &r->search_nodes, errp);
}
static void tensor_init(Object *obj)
{
    ((TensorRam *)obj)->search_nodes = 49;
    ((TensorRam *)obj)->adaptive = true;
}
static bool get_adaptive(Object *obj, Error **errp)
{
    return ((TensorRam *)obj)->adaptive;
}
static void set_adaptive(Object *obj, bool value, Error **errp)
{
    TensorRam *r = (TensorRam *)obj;
    if (host_memory_backend_mr_inited(&r->parent)) {
        error_setg(errp, "adaptive search is fixed after allocation");
        return;
    }
    r->adaptive = value;
}
static void tensor_init_class(ObjectClass *oc, void *data)
{
    object_class_property_add_str(oc, "image", get_image, set_image);
    object_class_property_add_bool(oc, "x-quiescent-reclaim", get_quiescent_reclaim,
                                   set_quiescent_reclaim);
    object_class_property_add(oc, "x-resident-target", "size", get_target, set_target, NULL, NULL);
    object_class_property_add(oc, "x-search-nodes", "uint32", get_search_nodes, set_search_nodes, NULL, NULL);
    object_class_property_add_bool(oc,"x-adaptive-search",get_adaptive,set_adaptive);
    MEMORY_BACKEND_CLASS(oc)->alloc = tensor_alloc;
}
static const TypeInfo tensor_info = {
    .name = "memory-backend-verdant-tensor",
    .parent = TYPE_MEMORY_BACKEND,
    .instance_size = sizeof(TensorRam),
    .instance_init = tensor_init,
    .instance_finalize = tensor_finalize,
    .class_init = tensor_init_class,
};
static void tensor_register(void) { type_register_static(&tensor_info); }
type_init(tensor_register);
