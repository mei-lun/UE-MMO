#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Bake a DNFM Spine 3.8 binary skeleton into UE-usable PNG frame sequences.

Renders the fighter Spine suite (fighter.skel.bytes + fighter.atlas.txt +
fighter.png, Spine 3.8.95) by:
  1. parsing the Spine 3.8 binary skeleton format (mirror of
     @pixi-spine/runtime-3.8 4.0.6 SkeletonBinary, the parser already used by
     DNFM_export tools/spine_inspector/audit_animations.cjs),
  2. sampling each animation at a fixed fps into full skeleton poses
     (bone local timelines with linear/stepped/bezier curves, attachment swap
     timelines, weighted mesh deform timelines, 2-bone IK constraints),
  3. computing weighted skinned mesh world vertices per frame,
  4. software rasterizing the textured triangles into RGBA frames with a
     shared view box across all animations.

Usage:
  py -I bake_spine_fighter.py --skel ... --atlas ... --png ... --out ...
     [--fps 30] [--verify reference.json] [--report-json path]
     [--anims 0,1,2] [--height 640] [--margin 8]

Standard library + Pillow only. Deterministic; re-runnable.
"""

import argparse
import hashlib
import json
import math
import os
import struct
import sys

from PIL import Image, ImageChops, ImageDraw

# ---------------------------------------------------------------------------
# Binary input (exact port of @pixi-spine/base BinaryInput)
# ---------------------------------------------------------------------------


class BinaryInput:
    __slots__ = ("data", "index", "strings")

    def __init__(self, data):
        self.data = data
        self.index = 0
        self.strings = []

    def read_byte(self):
        v = struct.unpack_from(">b", self.data, self.index)[0]
        self.index += 1
        return v

    def read_ubyte(self):
        v = self.data[self.index]
        self.index += 1
        return v

    def read_short(self):
        v = struct.unpack_from(">h", self.data, self.index)[0]
        self.index += 2
        return v

    def read_int32(self):
        v = struct.unpack_from(">i", self.data, self.index)[0]
        self.index += 4
        return v

    def read_int(self, optimize_positive):
        b = self.read_byte()
        result = b & 0x7F
        if (b & 0x80) != 0:
            b = self.read_byte()
            result |= (b & 0x7F) << 7
            if (b & 0x80) != 0:
                b = self.read_byte()
                result |= (b & 0x7F) << 14
                if (b & 0x80) != 0:
                    b = self.read_byte()
                    result |= (b & 0x7F) << 21
                    if (b & 0x80) != 0:
                        b = self.read_byte()
                        result |= (b & 0x7F) << 28
        if optimize_positive:
            return result
        return (result >> 1) ^ -(result & 1)

    def read_string_ref(self):
        index = self.read_int(True)
        return None if index == 0 else self.strings[index - 1]

    def read_string(self):
        byte_count = self.read_int(True)
        if byte_count == 0:
            return None
        if byte_count == 1:
            return ""
        byte_count -= 1
        raw = self.data[self.index:self.index + byte_count]
        self.index += byte_count
        return raw.decode("utf-8", "surrogateescape")

    def read_float(self):
        v = struct.unpack_from(">f", self.data, self.index)[0]
        self.index += 4
        return v

    def read_bool(self):
        return self.read_byte() != 0


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

TRANSFORM_MODES = ("normal", "only_translation", "no_rotation_or_reflection",
                   "no_scale", "no_scale_or_reflection")
BLEND_MODES = ("normal", "add", "multiply", "screen")
ATT_REGION, ATT_BOUNDINGBOX, ATT_MESH, ATT_LINKEDMESH, ATT_PATH, ATT_POINT, ATT_CLIPPING = range(7)


class BoneData:
    __slots__ = ("index", "name", "parent", "rotation", "x", "y", "scale_x", "scale_y",
                 "shear_x", "shear_y", "length", "transform_mode", "skin_required", "color")


class SlotData:
    __slots__ = ("index", "name", "bone_data", "color", "dark_color",
                 "attachment_name", "blend_mode")


class IkConstraintData:
    __slots__ = ("name", "order", "skin_required", "bones", "target", "mix",
                 "softness", "bend_direction", "compress", "stretch", "uniform")


class Skin:
    def __init__(self, name):
        self.name = name
        self.attachments = {}  # slotIndex -> {attachmentName: attachment}

    def set_attachment(self, slot_index, name, attachment):
        self.attachments.setdefault(slot_index, {})[name] = attachment

    def get_attachment(self, slot_index, name):
        return self.attachments.get(slot_index, {}).get(name)


class Attachment:
    __slots__ = ("type", "name", "path", "id")


class VertexAttachmentMixin:
    """Mixin providing vertex-attachment fields (no __slots__ conflict)."""


class RegionAttachment(Attachment):
    __slots__ = ("x", "y", "scale_x", "scale_y", "rotation", "width", "height",
                 "color", "region", "offset")

    def __init__(self, name):
        self.type = ATT_REGION
        self.name = name
        self.x = self.y = 0.0
        self.scale_x = self.scale_y = 1.0
        self.rotation = 0.0
        self.width = self.height = 0.0
        self.color = (1.0, 1.0, 1.0, 1.0)
        self.region = None
        self.offset = [0.0] * 8

    def update_offset(self):
        r = self.region
        region_scale_x = self.width / r["original_width"] * self.scale_x
        region_scale_y = self.height / r["original_height"] * self.scale_y
        local_x = -self.width / 2 * self.scale_x + r["offset_x"] * region_scale_x
        local_y = -self.height / 2 * self.scale_y + r["offset_y"] * region_scale_y
        local_x2 = local_x + r["width"] * region_scale_x
        local_y2 = local_y + r["height"] * region_scale_y
        radians = self.rotation * math.pi / 180.0
        cos, sin = math.cos(radians), math.sin(radians)
        local_x_cos = local_x * cos + self.x
        local_x_sin = local_x * sin
        local_y_cos = local_y * cos + self.y
        local_y_sin = local_y * sin
        local_x2_cos = local_x2 * cos + self.x
        local_x2_sin = local_x2 * sin
        local_y2_cos = local_y2 * cos + self.y
        local_y2_sin = local_y2 * sin
        off = self.offset
        off[0] = local_x_cos - local_y_sin
        off[1] = local_y_cos + local_x_sin
        off[2] = local_x_cos - local_y2_sin
        off[3] = local_y2_cos + local_x_sin
        off[4] = local_x2_cos - local_y2_sin
        off[5] = local_y2_cos + local_x2_sin
        off[6] = local_x2_cos - local_y_sin
        off[7] = local_y_cos + local_x2_sin


class VertexAttachment:
    __slots__ = ("bones", "vertices", "world_vertices_length", "deform_attachment")


class MeshAttachment(Attachment):
    __slots__ = ("color", "region_uvs", "triangles", "hull_length",
                 "parent_mesh", "region", "bones", "vertices",
                 "world_vertices_length", "deform_attachment")

    def __init__(self, name):
        self.type = ATT_MESH
        self.name = name
        self.color = (1.0, 1.0, 1.0, 1.0)
        self.bones = None
        self.vertices = None
        self.world_vertices_length = 0
        self.deform_attachment = None
        self.region_uvs = None
        self.triangles = None
        self.hull_length = 0
        self.parent_mesh = None
        self.region = None


class PointAttachment(Attachment):
    __slots__ = ("x", "y", "rotation", "color")

    def __init__(self, name):
        self.type = ATT_POINT
        self.name = name
        self.x = self.y = self.rotation = 0.0
        self.color = (1.0, 1.0, 1.0, 1.0)


class BoundingBoxAttachment(Attachment):
    __slots__ = ("bones", "vertices", "world_vertices_length", "deform_attachment")

    def __init__(self, name):
        self.type = ATT_BOUNDINGBOX
        self.name = name
        self.bones = None
        self.vertices = None
        self.world_vertices_length = 0
        self.deform_attachment = None


class PathAttachment(Attachment):
    __slots__ = ("bones", "vertices", "world_vertices_length", "deform_attachment",
                 "closed", "constant_speed", "lengths")

    def __init__(self, name):
        self.type = ATT_PATH
        self.name = name
        self.bones = None
        self.vertices = None
        self.world_vertices_length = 0
        self.deform_attachment = None
        self.closed = False
        self.constant_speed = False
        self.lengths = None


class ClippingAttachment(Attachment):
    __slots__ = ("end_slot", "bones", "vertices", "world_vertices_length",
                 "deform_attachment")

    def __init__(self, name):
        self.type = ATT_CLIPPING
        self.name = name
        self.end_slot = None
        self.bones = None
        self.vertices = None
        self.world_vertices_length = 0
        self.deform_attachment = None


# ---------------------------------------------------------------------------
# Timelines (ports of pixi-spine runtime-3.8 Animation.js)
# ---------------------------------------------------------------------------


def binary_search(values, target, step=1):
    low = 0
    high = len(values) // step - 2
    if high == 0:
        return step
    current = high >> 1
    while True:
        if values[(current + 1) * step] <= target:
            low = current + 1
        else:
            high = current
        if low == high:
            return (low + 1) * step
        current = (low + high) >> 1


class CurveTimeline:
    LINEAR, STEPPED, BEZIER = 0, 1, 2
    BEZIER_SIZE = 10 * 2 - 1

    def __init__(self, frame_count):
        self.curves = [0.0] * ((frame_count - 1) * self.BEZIER_SIZE)

    def set_linear(self, frame_index):
        self.curves[frame_index * self.BEZIER_SIZE] = self.LINEAR

    def set_stepped(self, frame_index):
        self.curves[frame_index * self.BEZIER_SIZE] = self.STEPPED

    def set_curve(self, frame_index, cx1, cy1, cx2, cy2):
        tmpx = (-cx1 * 2 + cx2) * 0.03
        tmpy = (-cy1 * 2 + cy2) * 0.03
        dddfx = ((cx1 - cx2) * 3 + 1) * 0.006
        dddfy = ((cy1 - cy2) * 3 + 1) * 0.006
        ddfx = tmpx * 2 + dddfx
        ddfy = tmpy * 2 + dddfy
        dfx = cx1 * 0.3 + tmpx + dddfx * 0.16666667
        dfy = cy1 * 0.3 + tmpy + dddfy * 0.16666667
        curves = self.curves
        i = frame_index * self.BEZIER_SIZE
        curves[i] = self.BEZIER
        i += 1
        x = dfx
        y = dfy
        n = frame_index * self.BEZIER_SIZE + self.BEZIER_SIZE - 1
        while i < n:
            curves[i] = x
            curves[i + 1] = y
            dfx += ddfx
            dfy += ddfy
            ddfx += dddfx
            ddfy += dddfy
            x += dfx
            y += dfy
            i += 2

    def get_curve_percent(self, frame_index, percent):
        if percent < 0.0:
            percent = 0.0
        elif percent > 1.0:
            percent = 1.0
        curves = self.curves
        i = frame_index * self.BEZIER_SIZE
        ctype = curves[i]
        if ctype == self.LINEAR:
            return percent
        if ctype == self.STEPPED:
            return 0.0
        i += 1
        x = 0.0
        start = i
        n = start + self.BEZIER_SIZE - 1
        while i < n:
            x = curves[i]
            if x >= percent:
                if i == start:
                    prev_x, prev_y = 0.0, 0.0
                else:
                    prev_x, prev_y = curves[i - 2], curves[i - 1]
                return prev_y + (curves[i + 1] - prev_y) * (percent - prev_x) / (x - prev_x)
            i += 2
        y = curves[i - 1]
        return y + (1 - y) * (percent - x) / (1 - x)


class RotateTimeline(CurveTimeline):
    ENTRIES = 2

    def __init__(self, frame_count):
        super().__init__(frame_count)
        self.frames = [0.0] * (frame_count * 2)
        self.bone_index = 0

    def set_frame(self, i, time, degrees):
        self.frames[i * 2] = time
        self.frames[i * 2 + 1] = degrees

    def apply(self, skeleton, time, alpha):
        frames = self.frames
        bone = skeleton.bones[self.bone_index]
        if not bone.active:
            return
        if time < frames[0]:
            return  # blend=replace, alpha=1: keep setup pose
        if time >= frames[len(frames) - 2]:
            r2 = frames[len(frames) - 1]
            r2 += bone.data.rotation - bone.rotation
            r2 -= (16384 - int(16384.499999999996 - r2 / 360)) * 360
            bone.rotation += r2 * alpha
            return
        frame = binary_search(frames, time, 2)
        prev_rotation = frames[frame - 1]
        frame_time = frames[frame]
        percent = self.get_curve_percent(frame // 2 - 1,
                                         1 - (time - frame_time) / (frames[frame - 2] - frame_time))
        r = frames[frame + 1] - prev_rotation
        r = prev_rotation + (r - (16384 - int(16384.499999999996 - r / 360)) * 360) * percent
        r += bone.data.rotation - bone.rotation
        bone.rotation += (r - (16384 - int(16384.499999999996 - r / 360)) * 360) * alpha


class TranslateTimeline(CurveTimeline):
    ENTRIES = 3

    def __init__(self, frame_count):
        super().__init__(frame_count)
        self.frames = [0.0] * (frame_count * 3)
        self.bone_index = 0

    def set_frame(self, i, time, x, y):
        self.frames[i * 3] = time
        self.frames[i * 3 + 1] = x
        self.frames[i * 3 + 2] = y

    def apply(self, skeleton, time, alpha):
        frames = self.frames
        bone = skeleton.bones[self.bone_index]
        if not bone.active:
            return
        if time < frames[0]:
            return
        if time >= frames[len(frames) - 3]:
            x = frames[len(frames) - 2]
            y = frames[len(frames) - 1]
        else:
            frame = binary_search(frames, time, 3)
            x = frames[frame - 2]
            y = frames[frame - 1]
            frame_time = frames[frame]
            percent = self.get_curve_percent(frame // 3 - 1,
                                             1 - (time - frame_time) / (frames[frame - 3] - frame_time))
            x += (frames[frame + 1] - x) * percent
            y += (frames[frame + 2] - y) * percent
        # blend replace, alpha=1 -> setup + value (bone currently at setup)
        bone.x = bone.data.x + x
        bone.y = bone.data.y + y


class ScaleTimeline(TranslateTimeline):
    def apply(self, skeleton, time, alpha):
        frames = self.frames
        bone = skeleton.bones[self.bone_index]
        if not bone.active:
            return
        if time < frames[0]:
            return
        if time >= frames[len(frames) - 3]:
            x = frames[len(frames) - 2] * bone.data.scale_x
            y = frames[len(frames) - 1] * bone.data.scale_y
        else:
            frame = binary_search(frames, time, 3)
            x = frames[frame - 2]
            y = frames[frame - 1]
            frame_time = frames[frame]
            percent = self.get_curve_percent(frame // 3 - 1,
                                             1 - (time - frame_time) / (frames[frame - 3] - frame_time))
            x = (x + (frames[frame + 1] - x) * percent) * bone.data.scale_x
            y = (y + (frames[frame + 2] - y) * percent) * bone.data.scale_y
        bone.scale_x = x
        bone.scale_y = y


class ShearTimeline(TranslateTimeline):
    def apply(self, skeleton, time, alpha):
        frames = self.frames
        bone = skeleton.bones[self.bone_index]
        if not bone.active:
            return
        if time < frames[0]:
            return
        if time >= frames[len(frames) - 3]:
            x = frames[len(frames) - 2]
            y = frames[len(frames) - 1]
        else:
            frame = binary_search(frames, time, 3)
            x = frames[frame - 2]
            y = frames[frame - 1]
            frame_time = frames[frame]
            percent = self.get_curve_percent(frame // 3 - 1,
                                             1 - (time - frame_time) / (frames[frame - 3] - frame_time))
            x += (frames[frame + 1] - x) * percent
            y += (frames[frame + 2] - y) * percent
        bone.shear_x = bone.data.shear_x + x
        bone.shear_y = bone.data.shear_y + y


class AttachmentTimeline:
    def __init__(self, frame_count):
        self.frames = [0.0] * frame_count
        self.attachment_names = [None] * frame_count
        self.slot_index = 0

    def apply(self, skeleton, time, alpha):
        frames = self.frames
        slot = skeleton.slots[self.slot_index]
        if not slot.bone.active:
            return
        if time < frames[0]:
            slot.set_attachment(
                skeleton.get_attachment(self.slot_index, slot.data.attachment_name))
            return
        if time >= frames[-1]:
            frame_index = len(frames) - 1
        else:
            frame_index = binary_search(frames, time, 1) - 1
        name = self.attachment_names[frame_index]
        slot.set_attachment(
            None if name is None else skeleton.get_attachment(self.slot_index, name))


class DeformTimeline(CurveTimeline):
    def __init__(self, frame_count):
        super().__init__(frame_count)
        self.frames = [0.0] * frame_count
        self.frame_vertices = [None] * frame_count
        self.slot_index = 0
        self.attachment = None

    def apply(self, skeleton, time, alpha):
        slot = skeleton.slots[self.slot_index]
        if not slot.bone.active:
            return
        slot_attachment = slot.attachment
        if not isinstance(slot_attachment, MeshAttachment) or \
                slot_attachment.deform_attachment is not self.attachment:
            return
        deform_array = slot.deform
        frame_vertices = self.frame_vertices
        vertex_count = len(frame_vertices[0])
        frames = self.frames
        if time < frames[0]:
            del deform_array[:]
            return
        deform = deform_array
        if len(deform) < vertex_count:
            deform.extend([0.0] * (vertex_count - len(deform)))
        if time >= frames[-1]:
            last = frame_vertices[-1]
            deform[:vertex_count] = last
            return
        frame = binary_search(frames, time, 1)
        prev_vertices = frame_vertices[frame - 1]
        next_vertices = frame_vertices[frame]
        frame_time = frames[frame]
        percent = self.get_curve_percent(frame - 1,
                                         1 - (time - frame_time) / (frames[frame - 1] - frame_time))
        for i in range(vertex_count):
            prev = prev_vertices[i]
            deform[i] = prev + (next_vertices[i] - prev) * percent


class DrawOrderTimeline:
    def __init__(self, frame_count):
        self.frames = [0.0] * frame_count
        self.draw_orders = [None] * frame_count

    def apply(self, skeleton, time, alpha):
        frames = self.frames
        if time < frames[0]:
            skeleton.draw_order = list(skeleton.slots)
            return
        if time >= frames[-1]:
            frame = len(frames) - 1
        else:
            frame = binary_search(frames, time, 1) - 1
        order = self.draw_orders[frame]
        if order is None:
            skeleton.draw_order = list(skeleton.slots)
        else:
            skeleton.draw_order = [skeleton.slots[i] for i in order]


class ColorTimeline(CurveTimeline):
    ENTRIES = 5

    def __init__(self, frame_count):
        super().__init__(frame_count)
        self.frames = [0.0] * (frame_count * 5)
        self.slot_index = 0

    def set_frame(self, i, time, r, g, b, a):
        base = i * 5
        self.frames[base:base + 5] = [time, r, g, b, a]

    def apply(self, skeleton, time, alpha):
        frames = self.frames
        slot = skeleton.slots[self.slot_index]
        if not slot.bone.active:
            return
        if time < frames[0]:
            slot.color = tuple(slot.data.color)
            return
        if time >= frames[len(frames) - 5]:
            i = len(frames)
            r, g, b, a = frames[i - 4], frames[i - 3], frames[i - 2], frames[i - 1]
        else:
            frame = binary_search(frames, time, 5)
            r, g, b, a = frames[frame - 4], frames[frame - 3], frames[frame - 2], frames[frame - 1]
            frame_time = frames[frame]
            percent = self.get_curve_percent(frame // 5 - 1,
                                             1 - (time - frame_time) / (frames[frame - 5] - frame_time))
            r += (frames[frame + 1] - r) * percent
            g += (frames[frame + 2] - g) * percent
            b += (frames[frame + 3] - b) * percent
            a += (frames[frame + 4] - a) * percent
        slot.color = (r, g, b, a)


class EventTimeline:  # parsed for completeness; events are non-visual
    def __init__(self, frame_count):
        self.frames = [0.0] * frame_count
        self.events = [None] * frame_count

    def apply(self, skeleton, time, alpha):
        return


class IkConstraintTimeline(CurveTimeline):
    ENTRIES = 6

    def __init__(self, frame_count):
        super().__init__(frame_count)
        self.frames = [0.0] * (frame_count * 6)
        self.ik_constraint_index = 0

    def set_frame(self, i, time, mix, softness, bend_direction, compress, stretch):
        base = i * 6
        self.frames[base:base + 6] = [time, mix, softness, float(bend_direction),
                                      1.0 if compress else 0.0, 1.0 if stretch else 0.0]

    def apply(self, skeleton, time, alpha):
        frames = self.frames
        constraint = skeleton.ik_constraints[self.ik_constraint_index]
        if not constraint.active:
            return
        if time < frames[0]:
            return
        if time >= frames[len(frames) - 6]:
            i = len(frames)
            mix = frames[i - 5]
            softness = frames[i - 4]
            bend_dir = int(frames[i - 3])
            compress = frames[i - 2] != 0
            stretch = frames[i - 1] != 0
        else:
            frame = binary_search(frames, time, 6)
            mix = frames[frame - 5]
            softness = frames[frame - 4]
            bend_dir = int(frames[frame - 3])
            compress = frames[frame - 2] != 0
            stretch = frames[frame - 1] != 0
            frame_time = frames[frame]
            percent = self.get_curve_percent(frame // 6 - 1,
                                             1 - (time - frame_time) / (frames[frame - 6] - frame_time))
            mix += (frames[frame + 1] - mix) * percent
            softness += (frames[frame + 2] - softness) * percent
        constraint.mix = mix
        constraint.softness = softness
        constraint.bend_direction = bend_dir
        constraint.compress = compress
        constraint.stretch = stretch


class TransformConstraintTimeline(CurveTimeline):
    ENTRIES = 5

    def __init__(self, frame_count):
        super().__init__(frame_count)
        self.frames = [0.0] * (frame_count * 5)
        self.transform_constraint_index = 0

    def apply(self, skeleton, time, alpha):
        raise NotImplementedError("transform constraint timelines not present in this asset")


# ---------------------------------------------------------------------------
# Skeleton binary parser (port of @pixi-spine/runtime-3.8 SkeletonBinary)
# ---------------------------------------------------------------------------


def rgba8888(v):
    return (((v >> 24) & 0xFF) / 255.0,
            ((v >> 16) & 0xFF) / 255.0,
            ((v >> 8) & 0xFF) / 255.0,
            (v & 0xFF) / 255.0)


def rgb888(v):
    return (((v >> 16) & 0xFF) / 255.0,
            ((v >> 8) & 0xFF) / 255.0,
            (v & 0xFF) / 255.0)


class SkeletonBinaryParser:
    BONE_ROTATE, BONE_TRANSLATE, BONE_SCALE, BONE_SHEAR = 0, 1, 2, 3
    SLOT_ATTACHMENT, SLOT_COLOR, SLOT_TWO_COLOR = 0, 1, 2
    PATH_POSITION, PATH_SPACING, PATH_MIX = 0, 1, 2
    CURVE_LINEAR, CURVE_STEPPED, CURVE_BEZIER = 0, 1, 2

    def __init__(self):
        self.scale = 1.0
        self.linked_meshes = []

    def read_skeleton_data(self, raw):
        inp = BinaryInput(raw)
        data = {
            "hash": inp.read_string(),
            "version": inp.read_string(),
            "x": inp.read_float(),
            "y": inp.read_float(),
            "width": inp.read_float(),
            "height": inp.read_float(),
            "fps": None,
            "bones": [],
            "slots": [],
            "ik_constraints": [],
            "transform_constraints": [],
            "path_constraints": [],
            "skins": [],
            "default_skin": None,
            "events": [],
            "animations": [],
        }
        if data["version"] == "3.8.75":
            print("warning: 3.8.75 is deprecated by the runtime", file=sys.stderr)
        nonessential = inp.read_bool()
        if nonessential:
            data["fps"] = inp.read_float()
            data["images_path"] = inp.read_string()
            data["audio_path"] = inp.read_string()
        n = inp.read_int(True)
        for _ in range(n):
            inp.strings.append(inp.read_string())

        n = inp.read_int(True)
        for i in range(n):
            bd = BoneData()
            bd.index = i
            bd.name = inp.read_string()
            bd.parent = None if i == 0 else data["bones"][inp.read_int(True)]
            bd.rotation = inp.read_float()
            bd.x = inp.read_float() * self.scale
            bd.y = inp.read_float() * self.scale
            bd.scale_x = inp.read_float()
            bd.scale_y = inp.read_float()
            bd.shear_x = inp.read_float()
            bd.shear_y = inp.read_float()
            bd.length = inp.read_float() * self.scale
            bd.transform_mode = inp.read_int(True)
            bd.skin_required = inp.read_bool()
            bd.color = rgba8888(inp.read_int32()) if nonessential else (0.61, 0.61, 0.61, 1.0)
            data["bones"].append(bd)

        n = inp.read_int(True)
        for i in range(n):
            sd = SlotData()
            sd.index = i
            sd.name = inp.read_string()
            sd.bone_data = data["bones"][inp.read_int(True)]
            sd.color = rgba8888(inp.read_int32())
            dark = inp.read_int32()
            sd.dark_color = None if dark == -1 else rgb888(dark)
            sd.attachment_name = inp.read_string_ref()
            sd.blend_mode = inp.read_int(True)
            data["slots"].append(sd)

        n = inp.read_int(True)
        for _ in range(n):
            cd = IkConstraintData()
            cd.name = inp.read_string()
            cd.order = inp.read_int(True)
            cd.skin_required = inp.read_bool()
            cd.bones = [data["bones"][inp.read_int(True)] for _ in range(inp.read_int(True))]
            cd.target = data["bones"][inp.read_int(True)]
            cd.mix = inp.read_float()
            cd.softness = inp.read_float() * self.scale
            cd.bend_direction = inp.read_byte()
            cd.compress = inp.read_bool()
            cd.stretch = inp.read_bool()
            cd.uniform = inp.read_bool()
            data["ik_constraints"].append(cd)

        n = inp.read_int(True)
        for _ in range(n):
            c = {"name": inp.read_string(), "order": inp.read_int(True),
                 "skin_required": inp.read_bool(),
                 "bones": [data["bones"][inp.read_int(True)] for _ in range(inp.read_int(True))]}
            c["target"] = data["bones"][inp.read_int(True)]
            for _f in range(9):
                c.setdefault("floats", []).append(inp.read_float())
            data["transform_constraints"].append(c)

        n = inp.read_int(True)
        for _ in range(n):
            c = {"name": inp.read_string(), "order": inp.read_int(True),
                 "skin_required": inp.read_bool(),
                 "bones": [data["bones"][inp.read_int(True)] for _ in range(inp.read_int(True))]}
            c["target_slot"] = data["slots"][inp.read_int(True)].name
            for _f in range(6):
                c.setdefault("floats", []).append(inp.read_float())
            data["path_constraints"].append(c)

        default_skin = self.read_skin(inp, data, True, nonessential)
        if default_skin is not None:
            data["default_skin"] = default_skin
            data["skins"].append(default_skin)
        for _ in range(inp.read_int(True)):
            data["skins"].append(self.read_skin(inp, data, False, nonessential))

        for lm in self.linked_meshes:
            skin = data["default_skin"] if lm["skin"] is None else next(
                (s for s in data["skins"] if s.name == lm["skin"]), None)
            if skin is None:
                raise RuntimeError("skin not found: %s" % lm["skin"])
            parent = skin.get_attachment(lm["slot_index"], lm["parent"])
            if parent is None:
                raise RuntimeError("parent mesh not found: %s" % lm["parent"])
            lm["mesh"].deform_attachment = parent if lm["inherit_deform"] else lm["mesh"]
            lm["mesh"].parent_mesh = parent
            lm["mesh"].bones = parent.bones
            lm["mesh"].vertices = parent.vertices
            lm["mesh"].world_vertices_length = parent.world_vertices_length
            lm["mesh"].region_uvs = parent.region_uvs
            lm["mesh"].triangles = parent.triangles
            lm["mesh"].hull_length = parent.hull_length
        self.linked_meshes = []

        n = inp.read_int(True)
        for _ in range(n):
            ev = {"name": inp.read_string_ref(), "int": inp.read_int(False),
                  "float": inp.read_float(), "string": inp.read_string()}
            audio = inp.read_string()
            if audio is not None:
                ev["audio"] = audio
                ev["volume"] = inp.read_float()
                ev["balance"] = inp.read_float()
            data["events"].append(ev)

        n = inp.read_int(True)
        for _ in range(n):
            data["animations"].append(self.read_animation(inp, inp.read_string(), data))
        return data

    def read_skin(self, inp, data, default_skin, nonessential):
        skin = None
        if default_skin:
            slot_count = inp.read_int(True)
            if slot_count == 0:
                return None
            skin = Skin("default")
        else:
            skin = Skin(inp.read_string_ref())
            for _ in range(inp.read_int(True)):
                inp.read_int(True)  # skin bone indices (unused: default skin only)
            for _c in range(3):
                for _ in range(inp.read_int(True)):
                    inp.read_int(True)
            slot_count = inp.read_int(True)
        for _ in range(slot_count):
            slot_index = inp.read_int(True)
            for _ in range(inp.read_int(True)):
                name = inp.read_string_ref()
                attachment = self.read_attachment(inp, data, skin, slot_index, name, nonessential)
                if attachment is not None:
                    skin.set_attachment(slot_index, name, attachment)
        return skin

    def read_attachment(self, inp, data, skin, slot_index, attachment_name, nonessential):
        name = inp.read_string_ref()
        if name is None:
            name = attachment_name
        type_index = inp.read_ubyte()
        scale = self.scale
        if type_index == ATT_REGION:
            path = inp.read_string_ref()
            att = RegionAttachment(name)
            att.x = inp.read_float() * scale
            att.y = inp.read_float() * scale
            att.scale_x = inp.read_float()
            att.scale_y = inp.read_float()
            att.rotation = inp.read_float()
            att.width = inp.read_float() * scale
            att.height = inp.read_float() * scale
            att.color = rgba8888(inp.read_int32())
            att.path = path if path is not None else name
            att.region = self.find_region(att.path)
            att.update_offset()
            return att
        if type_index == ATT_BOUNDINGBOX:
            att = BoundingBoxAttachment(name)
            vertex_count = inp.read_int(True)
            verts = self.read_vertices(inp, vertex_count)
            att.world_vertices_length = vertex_count << 1
            att.vertices = verts[0]
            att.bones = verts[1]
            if nonessential:
                inp.read_int32()
            return att
        if type_index == ATT_MESH:
            path = inp.read_string_ref()
            att = MeshAttachment(name)
            color = inp.read_int32()
            vertex_count = inp.read_int(True)
            uvs = [inp.read_float() for _ in range(vertex_count << 1)]
            triangles = [inp.read_short() for _ in range(inp.read_int(True))]
            verts = self.read_vertices(inp, vertex_count)
            hull_length = inp.read_int(True)
            width = height = 0
            if nonessential:
                for _ in range(inp.read_int(True)):
                    inp.read_short()
                width = inp.read_float()
                height = inp.read_float()
            att.color = rgba8888(color)
            att.bones = verts[1]
            att.vertices = verts[0]
            att.world_vertices_length = vertex_count << 1
            att.triangles = triangles
            att.region_uvs = uvs
            att.hull_length = hull_length << 1
            att.path = path if path is not None else name
            att.region = self.find_region(att.path)
            return att
        if type_index == ATT_LINKEDMESH:
            path = inp.read_string_ref()
            att = MeshAttachment(name)
            inp.read_int32()  # color
            skin_name = inp.read_string_ref()
            parent = inp.read_string_ref()
            inherit_deform = inp.read_bool()
            if nonessential:
                inp.read_float()
                inp.read_float()
            att.path = path if path is not None else name
            att.region = self.find_region(att.path)
            self.linked_meshes.append({"mesh": att, "skin": skin_name,
                                       "slot_index": slot_index, "parent": parent,
                                       "inherit_deform": inherit_deform})
            return att
        if type_index == ATT_PATH:
            att = PathAttachment(name)
            inp.read_bool()  # closed
            inp.read_bool()  # constant speed
            vertex_count = inp.read_int(True)
            verts = self.read_vertices(inp, vertex_count)
            for _ in range(vertex_count // 3):
                inp.read_float()
            if nonessential:
                inp.read_int32()
            att.world_vertices_length = vertex_count << 1
            att.vertices = verts[0]
            att.bones = verts[1]
            return att
        if type_index == ATT_POINT:
            att = PointAttachment(name)
            att.rotation = inp.read_float()
            att.x = inp.read_float() * scale
            att.y = inp.read_float() * scale
            if nonessential:
                att.color = rgba8888(inp.read_int32())
            return att
        if type_index == ATT_CLIPPING:
            att = ClippingAttachment(name)
            end_slot_index = inp.read_int(True)
            att.end_slot = data["slots"][end_slot_index]
            vertex_count = inp.read_int(True)
            verts = self.read_vertices(inp, vertex_count)
            if nonessential:
                inp.read_int32()
            att.world_vertices_length = vertex_count << 1
            att.vertices = verts[0]
            att.bones = verts[1]
            return att
        raise RuntimeError("unknown attachment type %d" % type_index)

    def find_region(self, path):
        region = self._regions.get(path)
        if region is None:
            raise RuntimeError("atlas region not found: %s" % path)
        return region

    _regions = {}

    def read_vertices(self, inp, vertex_count):
        vertices_length = vertex_count << 1
        if not inp.read_bool():
            return ([inp.read_float() for _ in range(vertices_length)], None)
        weights = []
        bones = []
        for _ in range(vertex_count):
            bone_count = inp.read_int(True)
            bones.append(bone_count)
            for _ in range(bone_count):
                bones.append(inp.read_int(True))
                weights.append(inp.read_float() * self.scale)
                weights.append(inp.read_float() * self.scale)
                weights.append(inp.read_float())
        return (weights, bones)

    def read_animation(self, inp, name, data):
        timelines = []
        duration = 0.0
        scale = self.scale

        n = inp.read_int(True)
        for _ in range(n):
            slot_index = inp.read_int(True)
            for _ in range(inp.read_int(True)):
                ttype = inp.read_byte()
                frame_count = inp.read_int(True)
                if ttype == self.SLOT_ATTACHMENT:
                    tl = AttachmentTimeline(frame_count)
                    tl.slot_index = slot_index
                    for fi in range(frame_count):
                        tl.frames[fi] = inp.read_float()
                        tl.attachment_names[fi] = inp.read_string_ref()
                    timelines.append(tl)
                    duration = max(duration, tl.frames[-1])
                elif ttype == self.SLOT_COLOR:
                    tl = ColorTimeline(frame_count)
                    tl.slot_index = slot_index
                    for fi in range(frame_count):
                        t = inp.read_float()
                        tl.set_frame(fi, t, *rgba8888(inp.read_int32()))
                        if fi < frame_count - 1:
                            self.read_curve(inp, fi, tl)
                    timelines.append(tl)
                    duration = max(duration, tl.frames[(frame_count - 1) * 5])
                elif ttype == self.SLOT_TWO_COLOR:
                    raise RuntimeError("two color timeline unsupported in this asset")
                else:
                    raise RuntimeError("unknown slot timeline %d" % ttype)

        n = inp.read_int(True)
        for _ in range(n):
            bone_index = inp.read_int(True)
            for _ in range(inp.read_int(True)):
                ttype = inp.read_byte()
                frame_count = inp.read_int(True)
                if ttype == self.BONE_ROTATE:
                    tl = RotateTimeline(frame_count)
                    tl.bone_index = bone_index
                    for fi in range(frame_count):
                        tl.set_frame(fi, inp.read_float(), inp.read_float())
                        if fi < frame_count - 1:
                            self.read_curve(inp, fi, tl)
                    timelines.append(tl)
                    duration = max(duration, tl.frames[(frame_count - 1) * 2])
                elif ttype in (self.BONE_TRANSLATE, self.BONE_SCALE, self.BONE_SHEAR):
                    cls = {self.BONE_TRANSLATE: TranslateTimeline,
                           self.BONE_SCALE: ScaleTimeline,
                           self.BONE_SHEAR: ShearTimeline}[ttype]
                    tl = cls(frame_count)
                    tl.bone_index = bone_index
                    tscale = scale if ttype == self.BONE_TRANSLATE else 1.0
                    for fi in range(frame_count):
                        tl.set_frame(fi, inp.read_float(),
                                     inp.read_float() * tscale, inp.read_float() * tscale)
                        if fi < frame_count - 1:
                            self.read_curve(inp, fi, tl)
                    timelines.append(tl)
                    duration = max(duration, tl.frames[(frame_count - 1) * 3])
                else:
                    raise RuntimeError("unknown bone timeline %d" % ttype)

        n = inp.read_int(True)
        for _ in range(n):
            index = inp.read_int(True)
            frame_count = inp.read_int(True)
            tl = IkConstraintTimeline(frame_count)
            tl.ik_constraint_index = index
            for fi in range(frame_count):
                tl.set_frame(fi, inp.read_float(), inp.read_float(),
                             inp.read_float() * scale, inp.read_byte(),
                             inp.read_bool(), inp.read_bool())
                if fi < frame_count - 1:
                    self.read_curve(inp, fi, tl)
            timelines.append(tl)
            duration = max(duration, tl.frames[(frame_count - 1) * 6])

        n = inp.read_int(True)
        for _ in range(n):
            index = inp.read_int(True)
            frame_count = inp.read_int(True)
            tl = TransformConstraintTimeline(frame_count)
            tl.transform_constraint_index = index
            for fi in range(frame_count):
                for _v in range(5):
                    inp.read_float()
                if fi < frame_count - 1:
                    self.read_curve(inp, fi, tl)
            raise RuntimeError("transform constraint timeline present but unsupported")

        n = inp.read_int(True)
        for _ in range(n):
            index = inp.read_int(True)
            for _ in range(inp.read_int(True)):
                ttype = inp.read_byte()
                frame_count = inp.read_int(True)
                if ttype == self.PATH_POSITION:
                    for fi in range(frame_count):
                        inp.read_float()
                        inp.read_float()
                        if fi < frame_count - 1:
                            self.read_curve(inp, fi, CurveTimeline(2))
                elif ttype == self.PATH_SPACING:
                    for fi in range(frame_count):
                        inp.read_float()
                        inp.read_float()
                        if fi < frame_count - 1:
                            self.read_curve(inp, fi, CurveTimeline(2))
                elif ttype == self.PATH_MIX:
                    for fi in range(frame_count):
                        for _v in range(3):
                            inp.read_float()
                        if fi < frame_count - 1:
                            self.read_curve(inp, fi, CurveTimeline(2))
                else:
                    raise RuntimeError("unknown path timeline %d" % ttype)
        if n:
            raise RuntimeError("path constraint timeline present but unsupported")

        n = inp.read_int(True)
        for _ in range(n):
            skin = data["skins"][inp.read_int(True)]
            for _ in range(inp.read_int(True)):
                slot_index = inp.read_int(True)
                for _ in range(inp.read_int(True)):
                    attachment = skin.get_attachment(slot_index, inp.read_string_ref())
                    weighted = attachment.bones is not None
                    vertices = attachment.vertices
                    deform_length = (len(vertices) // 3 * 2) if weighted else len(vertices)
                    frame_count = inp.read_int(True)
                    tl = DeformTimeline(frame_count)
                    tl.slot_index = slot_index
                    tl.attachment = attachment
                    for fi in range(frame_count):
                        t = inp.read_float()
                        end = inp.read_int(True)
                        if end == 0:
                            deform = [0.0] * deform_length if weighted else list(vertices)
                        else:
                            deform = [0.0] * deform_length
                            start = inp.read_int(True)
                            end += start
                            for v in range(start, end):
                                deform[v] = inp.read_float() * scale
                            if not weighted:
                                for v in range(len(deform)):
                                    deform[v] += vertices[v]
                        tl.frames[fi] = t
                        tl.frame_vertices[fi] = deform
                        if fi < frame_count - 1:
                            self.read_curve(inp, fi, tl)
                    timelines.append(tl)
                    duration = max(duration, tl.frames[frame_count - 1])

        draw_order_count = inp.read_int(True)
        if draw_order_count > 0:
            tl = DrawOrderTimeline(draw_order_count)
            slot_count = len(data["slots"])
            for i in range(draw_order_count):
                t = inp.read_float()
                offset_count = inp.read_int(True)
                draw_order = [-1] * slot_count
                unchanged = [0] * (slot_count - offset_count)
                original_index = 0
                unchanged_index = 0
                for _ in range(offset_count):
                    slot_index = inp.read_int(True)
                    while original_index != slot_index:
                        unchanged[unchanged_index] = original_index
                        unchanged_index += 1
                        original_index += 1
                    draw_order[original_index + inp.read_int(True)] = original_index
                    original_index += 1
                while original_index < slot_count:
                    unchanged[unchanged_index] = original_index
                    unchanged_index += 1
                    original_index += 1
                for ii in range(slot_count - 1, -1, -1):
                    if draw_order[ii] == -1:
                        unchanged_index -= 1
                        draw_order[ii] = unchanged[unchanged_index]
                tl.frames[i] = t
                tl.draw_orders[i] = draw_order
            timelines.append(tl)
            duration = max(duration, tl.frames[-1])

        event_count = inp.read_int(True)
        if event_count > 0:
            tl = EventTimeline(event_count)
            for i in range(event_count):
                t = inp.read_float()
                inp.read_int(True)  # event index
                inp.read_int(False)
                inp.read_float()
                if inp.read_bool():
                    inp.read_string()
                # audio volume/balance follow when eventData.audioPath != null
                tl.frames[i] = t
            timelines.append(tl)
            duration = max(duration, tl.frames[-1])

        return {"name": name, "timelines": timelines, "duration": duration}

    def read_curve(self, inp, frame_index, timeline):
        t = inp.read_byte()
        if t == self.CURVE_STEPPED:
            timeline.set_stepped(frame_index)
        elif t == self.CURVE_BEZIER:
            timeline.set_curve(frame_index, inp.read_float(), inp.read_float(),
                               inp.read_float(), inp.read_float())


# ---------------------------------------------------------------------------
# Runtime (bones, slots, skeleton, IK) - pure Spine y-up convention
# ---------------------------------------------------------------------------


class Bone:
    __slots__ = ("data", "skeleton", "parent", "children", "x", "y", "rotation",
                 "scale_x", "scale_y", "shear_x", "shear_y", "ax", "ay", "arotation",
                 "ascale_x", "ascale_y", "ashear_x", "ashear_y", "applied_valid",
                 "sorted", "active", "a", "b", "c", "d", "world_x", "world_y")

    def __init__(self, data, skeleton, parent):
        self.data = data
        self.skeleton = skeleton
        self.parent = parent
        self.children = []
        self.a = self.b = self.c = self.d = 0.0
        self.world_x = self.world_y = 0.0
        self.sorted = False
        self.active = False
        self.ax = self.ay = 0.0
        self.arotation = 0.0
        self.ascale_x = self.ascale_y = 0.0
        self.ashear_x = self.ashear_y = 0.0
        self.applied_valid = False
        self.set_to_setup_pose()

    def set_to_setup_pose(self):
        d = self.data
        self.x = d.x
        self.y = d.y
        self.rotation = d.rotation
        self.scale_x = d.scale_x
        self.scale_y = d.scale_y
        self.shear_x = d.shear_x
        self.shear_y = d.shear_y

    def update_world_transform(self):
        self.update_world_transform_with(self.x, self.y, self.rotation,
                                         self.scale_x, self.scale_y,
                                         self.shear_x, self.shear_y)

    def update_world_transform_with(self, x, y, rotation, scale_x, scale_y,
                                    shear_x, shear_y):
        """Verbatim port of pixi-spine runtime-3.8 Bone.updateWorldTransformWith
        with settings.yDown=true (the convention the DNFM web runtime uses).
        Matrix fields (a,b,c,d) follow the pixi core.Matrix layout the runtime
        writes; skeleton scale is 1 and sy = -1."""
        self.ax = x
        self.ay = y
        self.arotation = rotation
        self.ascale_x = scale_x
        self.ascale_y = scale_y
        self.ashear_x = shear_x
        self.ashear_y = shear_y
        self.applied_valid = True
        parent = self.parent
        rad = math.pi / 180.0

        def cos_deg(d):
            return math.cos(d * rad)

        def sin_deg(d):
            return math.sin(d * rad)

        sx = self.skeleton.scale_x
        sy = -self.skeleton.scale_y  # settings.yDown = true
        m = self
        if parent is None:
            rotation_y = rotation + 90 + shear_y
            m.a = cos_deg(rotation + shear_x) * scale_x * sx
            m.c = cos_deg(rotation_y) * scale_y * sx
            m.b = sin_deg(rotation + shear_x) * scale_x * sy
            m.d = sin_deg(rotation_y) * scale_y * sy
            m.world_x = x * sx
            m.world_y = y * sy
            return
        pa = parent.a
        pb = parent.c
        pc = parent.b
        pd = parent.d
        m.world_x = pa * x + pb * y + parent.world_x
        m.world_y = pc * x + pd * y + parent.world_y
        mode = self.data.transform_mode
        if mode == 0:  # normal
            rotation_y = rotation + 90 + shear_y
            la = cos_deg(rotation + shear_x) * scale_x
            lb = cos_deg(rotation_y) * scale_y
            lc = sin_deg(rotation + shear_x) * scale_x
            ld = sin_deg(rotation_y) * scale_y
            m.a = pa * la + pb * lc
            m.c = pa * lb + pb * ld
            m.b = pc * la + pd * lc
            m.d = pc * lb + pd * ld
            return
        if mode == 1:  # only translation
            rotation_y = rotation + 90 + shear_y
            m.a = cos_deg(rotation + shear_x) * scale_x
            m.c = cos_deg(rotation_y) * scale_y
            m.b = sin_deg(rotation + shear_x) * scale_x
            m.d = sin_deg(rotation_y) * scale_y
        elif mode == 2:  # no rotation or reflection
            s = pa * pa + pc * pc
            prx = 0.0
            if s > 0.0001:
                s = abs(pa * pd - pb * pc) / s
                pa /= self.skeleton.scale_x
                pc /= self.skeleton.scale_y
                pb = pc * s
                pd = pa * s
                prx = math.atan2(pc, pa) * (180.0 / math.pi)
            else:
                pa = 0.0
                pc = 0.0
                prx = 90 - math.atan2(pd, pb) * (180.0 / math.pi)
            rx = rotation + shear_x - prx
            ry = rotation + shear_y - prx + 90
            la = cos_deg(rx) * scale_x
            lb = cos_deg(ry) * scale_y
            lc = sin_deg(rx) * scale_x
            ld = sin_deg(ry) * scale_y
            m.a = pa * la - pb * lc
            m.c = pa * lb - pb * ld
            m.b = pc * la + pd * lc
            m.d = pc * lb + pd * ld
        else:  # no scale, no scale or reflection
            cos = cos_deg(rotation)
            sin = sin_deg(rotation)
            za = (pa * cos + pb * sin) / sx
            zc = (pc * cos + pd * sin) / sy
            s = math.sqrt(za * za + zc * zc)
            if s > 0.00001:
                s = 1.0 / s
            za *= s
            zc *= s
            s = math.sqrt(za * za + zc * zc)
            if mode == 3 and (pa * pd - pb * pc < 0) != (self.skeleton.scale_x < 0 != self.skeleton.scale_y < 0):
                s = -s
            r = math.pi / 2 + math.atan2(zc, za)
            zb = math.cos(r) * s
            zd = math.sin(r) * s
            la = cos_deg(shear_x) * scale_x
            lb = cos_deg(90 + shear_y) * scale_y
            lc = sin_deg(shear_x) * scale_x
            ld = sin_deg(90 + shear_y) * scale_y
            m.a = za * la + zb * lc
            m.c = za * lb + zb * ld
            m.b = zc * la + zd * lc
            m.d = zc * lb + zd * ld
        m.a *= sx
        m.c *= sx
        m.b *= sy
        m.d *= sy

    def update_applied_transform(self):
        self.applied_valid = True
        parent = self.parent
        m = self
        if parent is None:
            m.ax = m.world_x
            m.ay = m.world_y
            m.arotation = math.atan2(m.b, m.a) * (180.0 / math.pi)
            m.ascale_x = math.sqrt(m.a * m.a + m.b * m.b)
            m.ascale_y = math.sqrt(m.c * m.c + m.d * m.d)
            m.ashear_x = 0.0
            m.ashear_y = math.atan2(m.a * m.c + m.b * m.d, m.a * m.d - m.b * m.c) * (180.0 / math.pi)
            return
        pa, pb, pc, pd = parent.a, parent.b, parent.c, parent.d
        pid = 1.0 / (pa * pd - pb * pc)
        dx = m.world_x - parent.world_x
        dy = m.world_y - parent.world_y
        m.ax = dx * pd * pid - dy * pb * pid
        m.ay = dy * pa * pid - dx * pc * pid
        ia = pid * pd
        id_ = pid * pa
        ib = pid * pc
        ic = pid * pb
        ra = ia * m.a - ib * m.b
        rb = ia * m.c - ib * m.d
        rc = id_ * m.b - ic * m.a
        rd = id_ * m.d - ic * m.c
        m.ashear_x = 0.0
        m.ascale_x = math.sqrt(ra * ra + rc * rc)
        if m.ascale_x > 0.0001:
            det = ra * rd - rb * rc
            m.ascale_y = det / m.ascale_x
            m.ashear_y = math.atan2(ra * rb + rc * rd, det) * (180.0 / math.pi)
            m.arotation = math.atan2(rc, ra) * (180.0 / math.pi)
        else:
            m.ascale_x = 0.0
            m.ascale_y = math.sqrt(rb * rb + rd * rd)
            m.ashear_y = 0.0
            m.arotation = 90 - math.atan2(rd, rb) * (180.0 / math.pi)


class Slot:
    __slots__ = ("data", "bone", "color", "attachment", "deform", "blend_mode")

    def __init__(self, data, bone):
        self.data = data
        self.bone = bone
        self.color = tuple(data.color)
        self.attachment = None
        self.deform = []
        self.blend_mode = data.blend_mode
        self.set_to_setup_pose()

    def set_to_setup_pose(self):
        self.color = tuple(self.data.color)
        name = self.data.attachment_name
        self.set_attachment(
            None if name is None else self.bone.skeleton.get_attachment(self.data.index, name))

    def set_attachment(self, attachment):
        if self.attachment is attachment:
            return
        self.attachment = attachment
        self.deform = []


class IkConstraint:
    __slots__ = ("data", "bones", "target", "mix", "softness", "bend_direction",
                 "compress", "stretch", "uniform", "active", "sx", "sy")

    def __init__(self, data, skeleton):
        self.data = data
        self.bones = [skeleton.bones[b.index] for b in data.bones]
        self.target = skeleton.bones[data.target.index]
        self.mix = data.mix
        self.softness = data.softness
        self.bend_direction = data.bend_direction
        self.compress = data.compress
        self.stretch = data.stretch
        self.uniform = data.uniform
        self.active = False
        self.sx = 1.0
        self.sy = 1.0

    def apply(self):
        target = self.target
        bones = self.bones
        if len(bones) == 1:
            self.apply1(bones[0], target.world_x, target.world_y,
                        self.compress, self.stretch, self.uniform, self.mix)
        elif len(bones) == 2:
            self.apply2(bones[0], bones[1], target.world_x, target.world_y,
                        self.bend_direction, self.stretch, self.softness, self.mix)

    def apply1(self, bone, target_x, target_y, compress, stretch, uniform, alpha):
        if not bone.applied_valid:
            bone.update_applied_transform()
        p = bone.parent
        pa, pb, pc, pd = p.a, p.c, p.b, p.d  # pixi IkConstraint reads (a, c, b, d)
        rotation_ik = -bone.ashear_x - bone.arotation
        tx = ty = 0.0
        mode = bone.data.transform_mode
        if mode == 1:  # only translation
            tx = target_x - bone.world_x
            ty = target_y - bone.world_y
        else:
            if mode == 2:  # no rotation or reflection
                s = abs(pa * pd - pb * pc) / (pa * pa + pc * pc)
                sa = pa / self.sx
                sc = pc / self.sy
                pb = -sc * s * self.sx
                pd = sa * s * self.sy
                rotation_ik += math.atan2(sc, sa) * (180.0 / math.pi)
            x = target_x - bone.parent.world_x
            y = target_y - bone.parent.world_y
            d = pa * pd - pb * pc
            tx = (x * pd - y * pb) / d - bone.ax
            ty = (y * pa - x * pc) / d - bone.ay
        rotation_ik += math.atan2(ty, tx) * (180.0 / math.pi)
        if bone.ascale_x < 0:
            rotation_ik += 180
        if rotation_ik > 180:
            rotation_ik -= 360
        elif rotation_ik < -180:
            rotation_ik += 360
        sx = bone.ascale_x
        sy = bone.ascale_y
        if compress or stretch:
            if mode == 3 or mode == 4:
                tx = target_x - bone.world_x
                ty = target_y - bone.world_y
            b = bone.data.length * sx
            dd = math.sqrt(tx * tx + ty * ty)
            if (compress and dd < b) or (stretch and dd > b and b > 0.0001):
                s = (dd / b - 1) * alpha + 1
                sx *= s
                if uniform:
                    sy *= s
        bone.update_world_transform_with(bone.ax, bone.ay,
                                         bone.arotation + rotation_ik * alpha,
                                         sx, sy, bone.ashear_x, bone.ashear_y)

    def apply2(self, parent, child, target_x, target_y, bend_dir, stretch, softness, alpha):
        if alpha == 0:
            child.update_world_transform()
            return
        if not parent.applied_valid:
            parent.update_applied_transform()
        if not child.applied_valid:
            child.update_applied_transform()
        px, py = parent.ax, parent.ay
        psx = parent.ascale_x
        sx = psx
        psy = parent.ascale_y
        csx = child.ascale_x
        os1 = 0.0
        os2 = 0.0
        s2 = 0.0
        if psx < 0:
            psx = -psx
            os1 = 180.0
            s2 = -1.0
        else:
            os1 = 0.0
            s2 = 1.0
        if psy < 0:
            psy = -psy
            s2 = -s2
        if csx < 0:
            csx = -csx
            os2 = 180.0
        else:
            os2 = 0.0
        cx = child.ax
        cy = 0.0
        cwx = 0.0
        cwy = 0.0
        a, b, c, d = parent.a, parent.c, parent.b, parent.d  # pixi (a, c, b, d)
        u = abs(psx - psy) <= 0.0001
        if not u:
            cy = 0.0
            cwx = a * cx + parent.world_x
            cwy = c * cx + parent.world_y
        else:
            cy = child.ay
            cwx = a * cx + b * cy + parent.world_x
            cwy = c * cx + d * cy + parent.world_y
        pp = parent.parent
        a, b, c, d = pp.a, pp.c, pp.b, pp.d  # pixi (a, c, b, d)
        idet = 1.0 / (a * d - b * c)
        x = cwx - pp.world_x
        y = cwy - pp.world_y
        dx = (x * d - y * b) * idet - px
        dy = (y * a - x * c) * idet - py
        l1 = math.sqrt(dx * dx + dy * dy)
        l2 = child.data.length * csx
        if l1 < 0.0001:
            self.apply1(parent, target_x, target_y, False, stretch, False, alpha)
            child.update_world_transform_with(cx, cy, 0.0, child.ascale_x,
                                              child.ascale_y, child.ashear_x, child.ashear_y)
            return
        x = target_x - pp.world_x
        y = target_y - pp.world_y
        tx = (x * d - y * b) * idet - px
        ty = (y * a - x * c) * idet - py
        dd = tx * tx + ty * ty
        if softness != 0:
            softness *= psx * (csx + 1) / 2
            td = math.sqrt(dd)
            sd = td - l1 - l2 * psx + softness
            if sd > 0:
                p = min(1.0, sd / (softness * 2)) - 1
                p = (sd - softness * (1 - p * p)) / td
                tx -= p * tx
                ty -= p * ty
                dd = tx * tx + ty * ty
        if u:
            l2 *= psx
            cos = (dd - l1 * l1 - l2 * l2) / (2 * l1 * l2)
            if cos < -1:
                cos = -1
            elif cos > 1:
                cos = 1
                if stretch:
                    sx *= (math.sqrt(dd) / (l1 + l2) - 1) * alpha + 1
            a2 = math.acos(cos) * bend_dir
            a = l1 + l2 * cos
            b = l2 * math.sin(a2)
            a1 = math.atan2(ty * a - tx * b, tx * a + ty * b)
        else:
            a = psx * l2
            b = psy * l2
            aa = a * a
            bb = b * b
            ta = math.atan2(ty, tx)
            c = bb * l1 * l1 + aa * dd - aa * bb
            c1 = -2 * bb * l1
            c2 = bb - aa
            det = c1 * c1 - 4 * c2 * c
            if det >= 0:
                q = math.sqrt(det)
                if c1 < 0:
                    q = -q
                q = -(c1 + q) / 2
                r0 = q / c2
                r1 = c / q
                r = r0 if abs(r0) < abs(r1) else r1
                if r * r <= dd:
                    y = math.sqrt(dd - r * r) * bend_dir
                    a1 = ta - math.atan2(y, r)
                    a2 = math.atan2(y / psy, (r - l1) / psx)
                    self._finish2(parent, child, px, py, sx, cx, cy, os1, os2, s2, a1, a2, alpha)
                    return
            min_angle = math.pi
            min_x = l1 - a
            min_dist = min_x * min_x
            min_y = 0.0
            max_angle = 0.0
            max_x = l1 + a
            max_dist = max_x * max_x
            max_y = 0.0
            c = -a * l1 / (aa - bb)
            if -1 <= c <= 1:
                c = math.acos(c)
                x = a * math.cos(c) + l1
                y = b * math.sin(c)
                d = x * x + y * y
                if d < min_dist:
                    min_angle = c
                    min_dist = d
                    min_x = x
                    min_y = y
                if d > max_dist:
                    max_angle = c
                    max_dist = d
                    max_x = x
                    max_y = y
            if dd <= (min_dist + max_dist) / 2:
                a1 = ta - math.atan2(min_y * bend_dir, min_x)
                a2 = min_angle * bend_dir
            else:
                a1 = ta - math.atan2(max_y * bend_dir, max_x)
                a2 = max_angle * bend_dir
        self._finish2(parent, child, px, py, sx, cx, cy, os1, os2, s2, a1, a2, alpha)

    def _finish2(self, parent, child, px, py, sx, cx, cy, os1, os2, s2, a1, a2, alpha):
        os = math.atan2(cy, cx) * s2
        rotation = parent.arotation
        a1 = (a1 - os) * (180.0 / math.pi) + os1 - rotation
        if a1 > 180:
            a1 -= 360
        elif a1 < -180:
            a1 += 360
        parent.update_world_transform_with(px, py, rotation + a1 * alpha, sx,
                                           parent.ascale_y, 0.0, 0.0)
        rotation = child.arotation
        a2 = ((a2 + os) * (180.0 / math.pi) - child.ashear_x) * s2 + os2 - rotation
        if a2 > 180:
            a2 -= 360
        elif a2 < -180:
            a2 += 360
        child.update_world_transform_with(cx, cy, rotation + a2 * alpha, child.ascale_x,
                                          child.ascale_y, child.ashear_x, child.ashear_y)



class Skeleton:
    def __init__(self, data):
        self.data = data
        self.scale_x = 1.0
        self.scale_y = 1.0
        self.bones = []
        for bd in data["bones"]:
            parent = None if bd.parent is None else self.bones[bd.parent.index]
            bone = Bone(bd, self, parent)
            if parent is not None:
                parent.children.append(bone)
            self.bones.append(bone)
        self.slots = []
        for sd in data["slots"]:
            slot = Slot(sd, self.bones[sd.bone_data.index])
            self.slots.append(slot)
        self.ik_constraints = [IkConstraint(cd, self) for cd in data["ik_constraints"]]
        self.draw_order = list(self.slots)
        self._update_cache = []
        self._update_cache_reset = []
        self.update_cache()

    def get_attachment(self, slot_index, name):
        skin = self.data["default_skin"]
        if skin is not None:
            att = skin.get_attachment(slot_index, name)
            if att is not None:
                return att
        return None

    def update_cache(self):
        cache = self._update_cache
        cache.clear()
        self._update_cache_reset.clear()
        for bone in self.bones:
            bone.sorted = bone.data.skin_required
            bone.active = not bone.sorted
        # default skin has no skin bones (3.8 default skin); skip
        ik_constraints = self.ik_constraints
        constraint_count = len(ik_constraints)
        for i in range(constraint_count):
            for constraint in ik_constraints:
                if constraint.data.order == i:
                    self.sort_ik_constraint(constraint)
                    break
        for bone in self.bones:
            self.sort_bone(bone)

    def sort_bone(self, bone):
        if bone.sorted:
            return
        parent = bone.parent
        if parent is not None:
            self.sort_bone(parent)
        bone.sorted = True
        self._update_cache.append(bone)

    def sort_ik_constraint(self, constraint):
        target = constraint.target
        # no skin-required constraints in this asset (default skin only)
        constraint.active = target.active and not constraint.data.skin_required
        if not constraint.active:
            return
        self.sort_bone(target)
        constrained = constraint.bones
        parent = constrained[0]
        self.sort_bone(parent)
        if len(constrained) > 1:
            child = constrained[-1]
            if child not in self._update_cache:
                self._update_cache_reset.append(child)
        self._update_cache.append(constraint)
        self.sort_reset(parent.children)
        constrained[-1].sorted = True

    def sort_reset(self, bones):
        for bone in bones:
            bone.sorted = False
            self.sort_reset(bone.children)

    def set_to_setup_pose(self):
        for bone in self.bones:
            bone.set_to_setup_pose()
        for slot in self.slots:
            slot.set_to_setup_pose()

    def update_world_transform(self):
        for bone in self._update_cache_reset:
            bone.ax = bone.x
            bone.ay = bone.y
            bone.arotation = bone.rotation
            bone.ascale_x = bone.scale_x
            bone.ascale_y = bone.scale_y
            bone.ashear_x = bone.shear_x
            bone.ashear_y = bone.shear_y
            bone.applied_valid = True
        for item in self._update_cache:
            if isinstance(item, IkConstraint):
                item.apply()
            else:
                item.update_world_transform()

    def apply_animation(self, animation, time):
        for tl in animation["timelines"]:
            tl.apply(self, time, 1.0)


def compute_world_vertices_weighted(att, slot, out, deform):
    bones = att.bones
    vertices = att.vertices
    skeleton_bones = slot.bone.skeleton.bones
    v = 0     # index into bones[] (counts, then bone indices)
    t = 0     # index into vertices[] (triplets: boneIdx implied, x, y, weight)
    d = 0     # index into deform[] (pairs)
    w = 0
    total = att.world_vertices_length
    while w < total:
        wx = 0.0
        wy = 0.0
        n = bones[v]
        v += 1
        n += v
        while v < n:
            mat = skeleton_bones[bones[v]]
            dvx = deform[d] if deform else 0.0
            dvy = deform[d + 1] if deform else 0.0
            vx = vertices[t] + dvx
            vy = vertices[t + 1] + dvy
            weight = vertices[t + 2]
            # pixi Attachment.js: (a,b,c,d) = (mat.a, mat.c, mat.b, mat.d)
            wx += (vx * mat.a + vy * mat.c + mat.world_x) * weight
            wy += (vx * mat.b + vy * mat.d + mat.world_y) * weight
            v += 1
            t += 3
            d += 2
        out.append(wx)
        out.append(wy)
        w += 2


# ---------------------------------------------------------------------------
# Atlas parsing
# ---------------------------------------------------------------------------


def parse_atlas(txt_path):
    with open(txt_path, "r", encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    page = {"name": None, "width": 0, "height": 0, "regions": {}}
    region = None
    for line in lines:
        if not line.strip():
            region = None
            continue
        colon = line.find(":")
        if colon == -1:
            if page["name"] is None:
                page["name"] = line.strip()
                continue
            region = {"name": line.strip(), "rotate": 0, "x": 0, "y": 0, "width": 0,
                      "height": 0, "original_width": 0, "original_height": 0,
                      "offset_x": 0, "offset_y": 0, "index": -1}
            page["regions"][region["name"]] = region
            continue
        key = line[:colon].strip()
        vals = [v.strip() for v in line[colon + 1:].split(",")]
        if region is None:
            if key == "size":
                page["width"] = int(vals[0])
                page["height"] = int(vals[1])
            continue
        if key == "rotate":
            v = vals[0].lower()
            if v == "true":
                region["rotate"] = 90
            elif v == "false":
                region["rotate"] = 0
            else:
                region["rotate"] = float(vals[0]) % 360
        elif key == "xy":
            region["x"] = int(vals[0])
            region["y"] = int(vals[1])
        elif key == "size":
            region["width"] = int(vals[0])
            region["height"] = int(vals[1])
        elif key == "orig":
            region["original_width"] = int(vals[0])
            region["original_height"] = int(vals[1])
        elif key == "offset":
            region["offset_x"] = int(vals[0])
            region["offset_y"] = int(vals[1])
        elif key == "index":
            region["index"] = int(vals[0])
    for r in page["regions"].values():
        # packed dims on the page: swapped for 90/270
        if r["rotate"] in (90, 270):
            r["packed_w"], r["packed_h"] = r["height"], r["width"]
        else:
            r["packed_w"], r["packed_h"] = r["width"], r["height"]
        r["u"], r["v"] = r["x"], r["y"]
        r["u2"], r["v2"] = r["x"] + r["packed_w"], r["y"] + r["packed_h"]
    return page


def region_uv_to_page_px(region, u, v):
    """Map mesh regionUV (u right, v TOP-down over the unpacked region) to page
    pixel coords (y down), matching the pixi-spine TextureUvs/TextureMatrix
    math validated against the runtime oracle."""
    rot = region["rotate"]
    fx, fy = float(region["x"]), float(region["y"])
    pw, ph = float(region["packed_w"]), float(region["packed_h"])
    if rot == 0:
        return (fx + u * pw, fy + v * ph)
    if rot == 90:
        return (fx + v * pw, fy + (1.0 - u) * ph)
    if rot == 180:
        return (fx + (1.0 - u) * pw, fy + (1.0 - v) * ph)
    if rot == 270:
        return (fx + (1.0 - v) * pw, fy + u * ph)
    raise RuntimeError("unsupported atlas rotate %s" % rot)


# ---------------------------------------------------------------------------
# Rasterizer
# ---------------------------------------------------------------------------


def solve_affine(dst, src):
    """Return PIL AFFINE coeffs (a,b,c,d,e,f) mapping output pixel index space
    to input pixel index space so that output pixel centers map to the affine
    image of src given 3 correspondences dst[i]->src[i] (continuous coords)."""
    (x0, y0), (x1, y1), (x2, y2) = dst
    (u0, v0), (u1, v1), (u2, v2) = src
    det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if abs(det) < 1e-12:
        return None
    # continuous -> continuous
    a = ((u1 - u0) * (y2 - y0) - (u2 - u0) * (y1 - y0)) / det
    b = ((u2 - u0) * (x1 - x0) - (u1 - u0) * (x2 - x0)) / det
    c = u0 - a * x0 - b * y0
    d = ((v1 - v0) * (y2 - y0) - (v2 - v0) * (y1 - y0)) / det
    e = ((v2 - v0) * (x1 - x0) - (v1 - v0) * (x2 - x0)) / det
    f = v0 - d * x0 - e * y0
    # PIL maps integer output pixel (i,j) to (a*i + b*j + c, ...). We want
    # pixel center (i+0.5, j+0.5) -> continuous in, then index = cont - 0.5.
    return (a, b, c + a * 0.5 + b * 0.5 - 0.5,
            d, e, f + d * 0.5 + e * 0.5 - 0.5)


def draw_textured_triangles(frame, atlas_img, pts_px, uvs_px, triangles):
    """Rasterize a set of triangles. pts_px: frame-space continuous coords
    (pixel centers), uvs_px: atlas page pixel coords, triangles: index triples."""
    W, H = frame.size
    if len(triangles) < 3:
        return
    for k in range(0, len(triangles) - 2, 3):
        i0, i1, i2 = triangles[k], triangles[k + 1], triangles[k + 2]
        dx0, dy0 = pts_px[i0 * 2], pts_px[i0 * 2 + 1]
        dx1, dy1 = pts_px[i1 * 2], pts_px[i1 * 2 + 1]
        dx2, dy2 = pts_px[i2 * 2], pts_px[i2 * 2 + 1]
        # area in dest (for degenerate culling)
        area = (dx1 - dx0) * (dy2 - dy0) - (dx2 - dx0) * (dy1 - dy0)
        if abs(area) < 1e-9:
            continue
        minx = min(dx0, dx1, dx2)
        miny = min(dy0, dy1, dy2)
        maxx = max(dx0, dx1, dx2)
        maxy = max(dy0, dy1, dy2)
        bx0 = int(math.floor(minx))
        by0 = int(math.floor(miny))
        bx1 = int(math.ceil(maxx))
        by1 = int(math.ceil(maxy))
        if bx0 >= W or by0 >= H or bx1 <= 0 or by1 <= 0:
            continue
        if bx0 < 0 or by0 < 0 or bx1 > W or by1 > H:
            # clip against frame: skip triangles fully out, clamp others
            bx0c, by0c, bx1c, by1c = max(bx0, 0), max(by0, 0), min(bx1, W), min(by1, H)
        else:
            bx0c, by0c, bx1c, by1c = bx0, by0, bx1, by1
        bw = bx1c - bx0c
        bh = by1c - by0c
        if bw <= 0 or bh <= 0:
            continue
        coeffs = solve_affine(
            ((dx0 - bx0c, dy0 - by0c), (dx1 - bx0c, dy1 - by0c), (dx2 - bx0c, dy2 - by0c)),
            ((uvs_px[i0 * 2], uvs_px[i0 * 2 + 1]),
             (uvs_px[i1 * 2], uvs_px[i1 * 2 + 1]),
             (uvs_px[i2 * 2], uvs_px[i2 * 2 + 1])))
        if coeffs is None:
            continue
        patch = atlas_img.transform((bw, bh), Image.Transform.AFFINE, coeffs,
                                    resample=Image.Resampling.BILINEAR)
        mask = Image.new("L", (bw, bh), 0)
        mdraw = ImageDraw.Draw(mask)
        poly = [(dx0 - bx0c, dy0 - by0c), (dx1 - bx0c, dy1 - by0c), (dx2 - bx0c, dy2 - by0c)]
        mdraw.polygon(poly, fill=255, outline=255)
        alpha = patch.getchannel("A")
        patch.putalpha(ImageChops.multiply(alpha, mask))
        frame.alpha_composite(patch, (bx0c, by0c))


# ---------------------------------------------------------------------------
# Baking
# ---------------------------------------------------------------------------


class Baker:
    def __init__(self, skel_path, atlas_path, png_path, regions, data):
        self.skel_path = skel_path
        self.atlas_path = atlas_path
        self.png_path = png_path
        self.regions = regions
        self.data = data
        self.atlas_img = Image.open(png_path).convert("RGBA")

    def sample_frame(self, animation, time):
        skeleton = self.skeleton
        skeleton.set_to_setup_pose()
        if animation is not None:
            skeleton.apply_animation(animation, time)
        skeleton.update_world_transform()
        return skeleton

    def attachment_render_data(self, slot):
        """Return (pts_spine, uvs_px, triangles) for the slot's attachment, or None."""
        att = slot.attachment
        if att is None:
            return None
        if att.type == ATT_MESH:
            out = []
            deform = slot.deform if slot.deform else None
            compute_world_vertices_weighted(att, slot, out, deform)
            region = att.region
            uvs_px = []
            ruvs = att.region_uvs
            for i in range(0, len(ruvs), 2):
                uvs_px.extend(region_uv_to_page_px(region, ruvs[i], ruvs[i + 1]))
            return out, uvs_px, att.triangles
        if att.type == ATT_REGION:
            att.update_offset()
            off = att.offset
            bone = slot.bone
            ma, mb, mc, md = bone.a, bone.c, bone.b, bone.d  # pixi (a, c, b, d)
            x, y = bone.world_x, bone.world_y
            pts = []
            for k in range(4):
                ox, oy = off[k * 2], off[k * 2 + 1]
                pts.append(ox * ma + oy * mb + x)
                pts.append(ox * mc + oy * md + y)
            region = att.region
            # corner order: 1=BL 2=TL 3=TR 4=BR (spine y-up). Page-pixel uv per
            # rotate code, validated against the pixi-spine sprite path
            # (TextureUvs groupD8): code 6 => displayed image is packed rotated 90 CW.
            rot = region["rotate"]
            fx, fy = float(region["x"]), float(region["y"])
            pw, ph = float(region["packed_w"]), float(region["packed_h"])
            if rot == 0:
                uv = [(fx, fy + ph), (fx, fy), (fx + pw, fy), (fx + pw, fy + ph)]
            elif rot == 90:
                uv = [(fx + pw, fy + ph), (fx, fy + ph), (fx, fy), (fx + pw, fy)]
            elif rot == 180:
                uv = [(fx + pw, fy), (fx + pw, fy + ph), (fx, fy + ph), (fx, fy)]
            elif rot == 270:
                uv = [(fx, fy), (fx + pw, fy), (fx + pw, fy + ph), (fx, fy + ph)]
            else:
                raise RuntimeError("unsupported atlas rotate %s" % rot)
            uvs_px = [c for pair in uv for c in pair]
            tri = (0, 1, 2, 0, 2, 3)
            return pts, uvs_px, tri
        return None  # point/bbox/path/clipping: non-visual here


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--skel", required=True)
    ap.add_argument("--atlas", required=True)
    ap.add_argument("--png", required=True)
    ap.add_argument("--out", required=True, help="output directory for frames/manifest")
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--anims", default=None, help="comma list of animation names (default: all)")
    ap.add_argument("--height", type=int, default=640, help="frame height in px")
    ap.add_argument("--margin", type=float, default=8.0, help="view-box margin in px")
    ap.add_argument("--verify", default=None, help="oracle reference.json for numeric validation")
    ap.add_argument("--report-json", default=None, help="write full machine-readable report here")
    ap.add_argument("--max-frames", type=int, default=None, help="debug: cap frames per animation")
    args = ap.parse_args(argv)

    with open(args.skel, "rb") as fh:
        raw = fh.read()
    atlas_text_path = args.atlas
    page = parse_atlas(atlas_text_path)

    parser = SkeletonBinaryParser()
    parser._regions = page["regions"]
    data = parser.read_skeleton_data(raw)

    baker = Baker(args.skel, atlas_text_path, args.png, page["regions"], data)
    baker.skeleton = Skeleton(data)
    # IK scale (1.0) — pure spine convention

    anim_names = [a["name"] for a in data["animations"]]
    if args.anims:
        wanted = [s.strip() for s in args.anims.split(",")]
        anim_names = [n for n in anim_names if n in wanted]

    # ---- pass 1: view box over all frames/anims --------------------------
    min_x = min_y = float("inf")
    max_x = max_y = float("-inf")
    plan = []
    for name in anim_names:
        anim = next(a for a in data["animations"] if a["name"] == name)
        n_frames = int(round(anim["duration"] * args.fps))
        times = [min(k / args.fps, anim["duration"]) for k in range(n_frames + 1)]
        if args.max_frames:
            times = times[:args.max_frames]
        plan.append((anim, times))
    for anim, times in plan:
        for t in times:
            sk = baker.sample_frame(anim, t)
            for slot in sk.slots:
                rd = baker.attachment_render_data(slot)
                if rd is None:
                    continue
                pts = rd[0]
                for i in range(0, len(pts), 2):
                    x = pts[i]
                    y = pts[i + 1]
                    if x < min_x:
                        min_x = x
                    if x > max_x:
                        max_x = x
                    if y < min_y:
                        min_y = y
                    if y > max_y:
                        max_y = y
    if min_x is float("inf") or max_x == float("-inf"):
        raise RuntimeError("no visible attachment geometry found")
    span_x = max_x - min_x
    span_y = max_y - min_y
    scale = (args.height - 2 * args.margin) / span_y
    out_w = int(math.ceil(span_x * scale + 2 * args.margin))
    out_h = args.height
    view = {"min_x": min_x, "min_y": min_y, "span_x": span_x, "span_y": span_y,
            "scale": scale, "width": out_w, "height": out_h}

    def to_px(pts):
        # world (pixi yDown) is already screen-oriented (y grows downward);
        # the view box was measured in the same space.
        outp = []
        for i in range(0, len(pts), 2):
            outp.append((pts[i] - min_x) * scale + args.margin)
            outp.append((pts[i + 1] - min_y) * scale + args.margin)
        return outp

    # ---- optional numeric verification against pixi-spine oracle ---------
    verify_summary = None
    if args.verify:
        with open(args.verify, "r", encoding="utf-8") as fh:
            oracle = json.load(fh)
        verify_summary = verify_against_oracle(baker, oracle, page)

    # ---- pass 2: rasterize ----------------------------------------------
    frames_root = os.path.join(args.out, "frames")
    os.makedirs(frames_root, exist_ok=True)
    manifest = {
        "generator": "Scripts/DNFM/bake_spine_fighter.py",
        "source": {
            "skel": os.path.abspath(args.skel), "skel_sha256": sha256_file(args.skel),
            "atlas": os.path.abspath(args.atlas), "atlas_sha256": sha256_file(args.atlas),
            "png": os.path.abspath(args.png), "png_sha256": sha256_file(args.png),
            "spine_version": data["version"],
        },
        "skeleton": {
            "bones": len(data["bones"]),
            "slots": len(data["slots"]),
            "ik_constraints": [c.name for c in data["ik_constraints"]],
            "attachment_type_counts": count_attachment_types(data),
        },
        "view_box": view,
        "fps": args.fps,
        "animations": [],
        "unsupported_effects": collect_unsupported(data),
    }
    empty_frames = []
    for anim, times in plan:
        adir = os.path.join(frames_root, anim["name"])
        os.makedirs(adir, exist_ok=True)
        frames_info = []
        for k, t in enumerate(times):
            sk = baker.sample_frame(anim, t)
            frame = Image.new("RGBA", (out_w, out_h), (0, 0, 0, 0))
            drawn = 0
            for slot in sk.draw_order:
                rd = baker.attachment_render_data(slot)
                if rd is None:
                    continue
                pts, uvs_px, tris = rd
                pts_px = to_px(pts)
                draw_textured_triangles(frame, baker.atlas_img, pts_px, uvs_px, tris)
                drawn += 1
            alpha_hist = frame.getchannel("A").histogram()
            nonzero = frame.size[0] * frame.size[1] - alpha_hist[0]
            fname = "frame_%04d.png" % k
            frame.save(os.path.join(adir, fname), compress_level=6)
            frames_info.append({"file": fname, "time": t,
                                "opaque_pixels": nonzero,
                                "attachments_drawn": drawn})
            if nonzero == 0:
                empty_frames.append("%s/%s" % (anim["name"], fname))
        manifest["animations"].append({
            "name": anim["name"],
            "duration_seconds": anim["duration"],
            "fps": args.fps,
            "frame_count": len(times),
            "frame_times_sample": "t = min(k/fps, duration), k=0..N",
            "timeline_type_counts": count_timeline_types(anim),
            "frames": frames_info,
        })
        print("baked anim %s: %d frames" % (anim["name"], len(times)), flush=True)
    manifest["empty_frames"] = empty_frames

    # ---- contact sheet ----------------------------------------------------
    sheet_path = os.path.join(args.out, "contact_sheet.png")
    make_contact_sheet(frames_root, manifest, sheet_path)
    manifest["contact_sheet"] = os.path.abspath(sheet_path)

    if verify_summary is not None:
        manifest["verification_vs_pixi_spine_oracle"] = verify_summary
    manifest_path = os.path.join(args.out, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2, ensure_ascii=False)
    print("manifest:", manifest_path)
    if args.report_json:
        with open(args.report_json, "w", encoding="utf-8") as fh:
            json.dump(manifest, fh, indent=2, ensure_ascii=False)
    if empty_frames:
        print("WARNING empty frames:", empty_frames, file=sys.stderr)
    return 0


def count_attachment_types(data):
    counts = {}
    skin = data["default_skin"]
    for by_slot in skin.attachments.values():
        for att in by_slot.values():
            counts[att.type] = counts.get(att.type, 0) + 1
    names = {ATT_REGION: "region", ATT_BOUNDINGBOX: "boundingbox", ATT_MESH: "mesh",
             ATT_LINKEDMESH: "linkedmesh", ATT_PATH: "path", ATT_POINT: "point",
             ATT_CLIPPING: "clipping"}
    return {names[k]: v for k, v in sorted(counts.items())}


def count_timeline_types(anim):
    counts = {}
    for tl in anim["timelines"]:
        key = type(tl).__name__
        counts[key] = counts.get(key, 0) + 1
    return counts


def collect_unsupported(data):
    """Effects present in the asset but not rendered (must be empty for a full bake)."""
    unsupported = []
    skin = data["default_skin"]
    types = {}
    for by_slot in skin.attachments.values():
        for att in by_slot.values():
            types[att.type] = types.get(att.type, 0) + 1
    if types.get(ATT_CLIPPING):
        unsupported.append({"effect": "clipping attachments", "count": types[ATT_CLIPPING],
                            "impact": "pixels outside clip polys are drawn instead of masked"})
    for slot in data["slots"]:
        if slot.blend_mode != 0:
            unsupported.append({"effect": "blend mode %s on slot %s" % (BLEND_MODES[slot.blend_mode], slot.name)})
        if slot.dark_color is not None:
            unsupported.append({"effect": "two-color (dark) tint on slot %s" % slot.name})
    for anim in data["animations"]:
        for tl in anim["timelines"]:
            if isinstance(tl, TransformConstraintTimeline) or type(tl).__name__ == "EventTimeline":
                pass
    if data["transform_constraints"]:
        unsupported.append({"effect": "transform constraints (setup values applied, no timelines expected)"})
    if data["path_constraints"]:
        unsupported.append({"effect": "path constraints"})
    return unsupported


def make_contact_sheet(frames_root, manifest, out_path, cols=10, cell_w=150):
    from PIL import ImageFont
    anims = [a for a in manifest["animations"] if a["frame_count"] > 0]
    if not anims:
        return
    first = Image.open(os.path.join(frames_root, anims[0]["name"], anims[0]["frames"][0]["file"]))
    aspect = first.size[1] / first.size[0]
    cell_h = int(cell_w * aspect)
    label_h = 18
    rows = len(anims)
    sheet = Image.new("RGBA", (cols * cell_w, rows * (cell_h + label_h) + label_h), (24, 24, 28, 255))
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.load_default(12)
    except TypeError:
        font = ImageFont.load_default()
    draw.text((4, 2), "fighter Spine bake contact sheet (every %s frame per row)" %
              max(1, anims[0]["frame_count"] // cols), fill=(220, 220, 220, 255), font=font)
    for r, anim in enumerate(anims):
        y0 = label_h + r * (cell_h + label_h)
        count = anim["frame_count"]
        for c in range(cols):
            idx = min(count - 1, int(round(c * (count - 1) / max(1, cols - 1))))
            path = os.path.join(frames_root, anim["name"], anim["frames"][idx]["file"])
            img = Image.open(path).convert("RGBA")
            img.thumbnail((cell_w - 2, cell_h - 2), Image.Resampling.LANCZOS)
            cell = Image.new("RGBA", (cell_w, cell_h), (40, 40, 46, 255))
            cell.alpha_composite(img, ((cell_w - img.size[0]) // 2, (cell_h - img.size[1]) // 2))
            sheet.alpha_composite(cell, (c * cell_w, y0))
        draw.text((4, y0 + cell_h + 2), "anim %s  (%d frames, %.2fs)" %
                  (anim["name"], anim["frame_count"], anim["duration_seconds"]),
                  fill=(180, 220, 180, 255), font=font)
    sheet.save(out_path)


def verify_against_oracle(baker, oracle, page):
    """Compare bone world matrices and attachment world vertices against the
    pixi-spine oracle dump. The renderer now uses the identical yDown runtime
    convention, so matrices and vertices compare directly (no sign flips)."""
    max_bone_err = 0.0
    max_vert_err = 0.0
    max_vert_rel = 0.0
    vert_compared = 0
    checked_poses = 0
    details = []
    for pose in oracle["poses"]:
        anim_name = pose["animation"]
        if anim_name is None:
            anim = None
        else:
            anim = next((a for a in baker.data["animations"] if a["name"] == anim_name), None)
            if anim is None:
                continue
        sk = baker.sample_frame(anim, pose["time"])
        checked_poses += 1
        pose_bone_err = 0.0
        for i, (oa, ob, oc, od, otx, oty) in enumerate(pose["bones"]):
            bone = sk.bones[i]
            errs = (abs(bone.a - oa), abs(bone.b - ob), abs(bone.c - oc),
                    abs(bone.d - od), abs(bone.world_x - otx), abs(bone.world_y - oty))
            pose_bone_err = max(pose_bone_err, max(errs))
        max_bone_err = max(max_bone_err, pose_bone_err)
        pose_vert_err = 0.0
        for slot_rec in pose["slots"]:
            slot = sk.slots[slot_rec["slotIndex"]]
            wv = slot_rec.get("worldVertices")
            if wv is None:
                continue
            rd = baker.attachment_render_data(slot)
            if rd is None:
                continue
            pts = rd[0]
            for j in range(0, min(len(pts), len(wv)), 2):
                # same yDown convention on both sides
                ex = abs(pts[j] - wv[j])
                ey = abs(pts[j + 1] - wv[j + 1])
                e = max(ex, ey)
                scale_ref = max(1.0, abs(wv[j]), abs(wv[j + 1]))
                max_vert_rel = max(max_vert_rel, e / scale_ref)
                pose_vert_err = max(pose_vert_err, e)
                vert_compared += 1
            max_vert_err = max(max_vert_err, pose_vert_err)
        details.append({"animation": anim_name, "time": pose["time"],
                        "max_bone_matrix_err": pose_bone_err,
                        "max_world_vertex_err": pose_vert_err})
    return {
        "oracle": "pixi-spine runtime-3.8 dump (tools reference.json)",
        "poses_checked": checked_poses,
        "world_vertex_components_compared": vert_compared,
        "max_abs_bone_matrix_component_err": max_bone_err,
        "max_abs_world_vertex_err": max_vert_err,
        "max_rel_world_vertex_err": max_vert_rel,
        "note": "renderer uses the identical pixi-spine yDown runtime convention; direct comparison",
        "per_pose": details,
    }


if __name__ == "__main__":
    sys.exit(main())
