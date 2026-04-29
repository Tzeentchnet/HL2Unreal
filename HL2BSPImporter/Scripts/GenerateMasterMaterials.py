"""
Generate the HL2BSPImporter master materials inside Unreal Editor.

Run from the Unreal Editor Python console, an Editor Utility, or:

    UnrealEditor-Cmd.exe <Project>.uproject -run=PythonScript -script=".../HL2BSPImporter/Scripts/GenerateMasterMaterials.py"

This script intentionally uses editor-only APIs. It creates graph assets under
/HL2BSPImporter/MasterMaterials and saves them so the C++ material synthesizer
can load the configured parent materials by soft-object path.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

try:
    import unreal
except ImportError as exc:  # Allows local syntax checks outside Unreal.
    unreal = None  # type: ignore[assignment]
    _IMPORT_ERROR = exc
else:
    _IMPORT_ERROR = None


TARGET_ROOT = "/HL2BSPImporter/MasterMaterials"
OVERWRITE_EXISTING = False


TEXTURE_PARAMS = (
    "BaseColor",
    "BaseColor2",
    "Normal",
    "Normal2",
    "NormalAlpha",
    "Normal2Alpha",
    "Detail",
    "BlendModulate",
    "EmissiveColor",
)

TEXTURE_CUBE_PARAMS = ("Cubemap",)

SCALAR_DEFAULTS: Dict[str, float] = {
    "AlphaTestRef": 0.5,
    "DetailScale": 1.0,
    "EmissiveStrength": 0.0,
    "OpacityScalar": 1.0,
    "Phong": 0.0,
    "PhongExponent": 5.0,
    "PhongBoost": 1.0,
    "BaseColorUVRotate": 0.0,
    "NormalUVRotate": 0.0,
    "DetailBlendMode": 0.0,
    "DetailBlendFactor": 1.0,
}

VECTOR_DEFAULTS: Dict[str, Tuple[float, float, float, float]] = {
    "EmissiveTint": (1.0, 1.0, 1.0, 1.0),
    "BaseColorTint": (1.0, 1.0, 1.0, 1.0),
    "BaseColor2Tint": (1.0, 1.0, 1.0, 1.0),
    "EnvmapTint": (1.0, 1.0, 1.0, 1.0),
    "BaseColorUVScale": (1.0, 1.0, 0.0, 0.0),
    "BaseColorUVOffset": (0.0, 0.0, 0.0, 0.0),
    "NormalUVScale": (1.0, 1.0, 0.0, 0.0),
    "NormalUVOffset": (0.0, 0.0, 0.0, 0.0),
}


@dataclass(frozen=True)
class MaterialSpec:
    name: str
    domain: str
    blend: str
    shading: str
    variant: str


MATERIALS: Sequence[MaterialSpec] = (
    MaterialSpec("M_HL2_Lit", "surface", "opaque", "default_lit", "lit"),
    MaterialSpec("M_HL2_LitMasked", "surface", "masked", "default_lit", "masked"),
    MaterialSpec("M_HL2_LitTranslucent", "surface", "translucent", "default_lit", "translucent"),
    MaterialSpec("M_HL2_LitDecal", "deferred_decal", "translucent", "default_lit", "decal"),
    MaterialSpec("M_HL2_WorldVertexBlend", "surface", "opaque", "default_lit", "world_blend"),
    MaterialSpec("M_HL2_VertexLit", "surface", "opaque", "default_lit", "vertex_lit"),
    MaterialSpec("M_HL2_Unlit", "surface", "opaque", "unlit", "unlit"),
)


class MaterialBuildError(RuntimeError):
    pass


def _require_unreal() -> Any:
    if unreal is None:
        raise MaterialBuildError(
            "This script must run inside Unreal Editor Python. "
            f"Import error was: {_IMPORT_ERROR!r}"
        )
    return unreal


def log(message: str) -> None:
    if unreal is not None:
        unreal.log(f"[HL2 MasterMaterials] {message}")
    else:
        print(f"[HL2 MasterMaterials] {message}")


def warn(message: str) -> None:
    if unreal is not None:
        unreal.log_warning(f"[HL2 MasterMaterials] {message}")
    else:
        print(f"[HL2 MasterMaterials][warning] {message}")


def enum_value(enum_name: str, candidates: Iterable[str]) -> Any:
    ue = _require_unreal()
    enum_type = getattr(ue, enum_name, None)
    if enum_type is None:
        raise MaterialBuildError(f"Unreal enum {enum_name} is not available")
    for candidate in candidates:
        value = getattr(enum_type, candidate, None)
        if value is not None:
            return value
    raise MaterialBuildError(f"None of {tuple(candidates)!r} exists on unreal.{enum_name}")


def set_prop(obj: Any, names: Sequence[str], value: Any, required: bool = True) -> bool:
    for name in names:
        try:
            obj.set_editor_property(name, value)
            return True
        except Exception:
            continue
    if required:
        raise MaterialBuildError(f"Failed to set any of {names!r} on {obj}")
    return False


def set_if_possible(obj: Any, names: Sequence[str], value: Any) -> bool:
    return set_prop(obj, names, value, required=False)


def linear_color(value: Tuple[float, float, float, float]) -> Any:
    ue = _require_unreal()
    return ue.LinearColor(value[0], value[1], value[2], value[3])


def load_asset(paths: Sequence[str]) -> Optional[Any]:
    ue = _require_unreal()
    for path in paths:
        asset = ue.EditorAssetLibrary.load_asset(path)
        if asset is not None:
            return asset
    return None


def default_texture(kind: str) -> Optional[Any]:
    if kind == "normal":
        return load_asset((
            "/Engine/EngineResources/DefaultNormal.DefaultNormal",
            "/Engine/EngineResources/DefaultTexture.DefaultTexture",
        ))
    if kind == "white":
        return load_asset((
            "/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture",
            "/Engine/EngineResources/DefaultTexture.DefaultTexture",
        ))
    return load_asset((
        "/Engine/EngineResources/DefaultTexture.DefaultTexture",
        "/Engine/EngineMaterials/DefaultDiffuse.DefaultDiffuse",
    ))


def default_cube_texture() -> Optional[Any]:
    return load_asset((
        "/Engine/EngineResources/DefaultTextureCube.DefaultTextureCube",
        "/Engine/EngineResources/DefaultTexture.DefaultTexture",
    ))


def material_property(name: str) -> Any:
    return enum_value("MaterialProperty", (name, name.upper()))


def blend_mode(key: str) -> Any:
    return enum_value("BlendMode", {
        "opaque": ("BLEND_OPAQUE", "BLEND_Opaque"),
        "masked": ("BLEND_MASKED", "BLEND_Masked"),
        "translucent": ("BLEND_TRANSLUCENT", "BLEND_Translucent"),
    }[key])


def material_domain(key: str) -> Any:
    return enum_value("MaterialDomain", {
        "surface": ("MD_SURFACE", "MD_Surface"),
        "deferred_decal": ("MD_DEFERRED_DECAL", "MD_DeferredDecal"),
    }[key])


def shading_model(key: str) -> Any:
    return enum_value("MaterialShadingModel", {
        "default_lit": ("MSM_DEFAULT_LIT", "MSM_DefaultLit"),
        "unlit": ("MSM_UNLIT", "MSM_Unlit"),
    }[key])


class GraphBuilder:
    def __init__(self, material: Any, spec: MaterialSpec):
        self.ue = _require_unreal()
        self.lib = self.ue.MaterialEditingLibrary
        self.material = material
        self.spec = spec
        self.x = -1600
        self.y = -800
        self.parameters: Dict[str, Any] = {}

    def node(self, class_name: str, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        cls = getattr(self.ue, class_name, None)
        if cls is None:
            raise MaterialBuildError(f"unreal.{class_name} is unavailable")
        if x is None:
            x = self.x
            self.x += 180
        if y is None:
            y = self.y
            self.y += 80
            if self.y > 900:
                self.y = -800
        return self.lib.create_material_expression(self.material, cls, x, y)

    def connect(self, source: Any, source_output: str, target: Any, target_input: str) -> bool:
        outputs = (source_output, "", "RGB", "RGBA") if source_output else ("", "RGB", "RGBA")
        for output_name in outputs:
            try:
                if self.lib.connect_material_expressions(source, output_name, target, target_input):
                    return True
            except Exception:
                continue
        warn(f"Failed to connect {source} output {source_output!r} to {target} input {target_input!r}")
        return False

    def connect_property(self, source: Any, source_output: str, property_name: str) -> bool:
        prop = material_property(property_name)
        outputs = (source_output, "", "RGB", "RGBA") if source_output else ("", "RGB", "RGBA")
        for output_name in outputs:
            try:
                if self.lib.connect_material_property(source, output_name, prop):
                    return True
            except Exception:
                continue
        warn(f"Failed to connect {source} output {source_output!r} to material property {property_name}")
        return False

    def scalar(self, name: str, default: float, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        expr = self.node("MaterialExpressionScalarParameter", x, y)
        set_prop(expr, ("parameter_name",), name)
        set_if_possible(expr, ("default_value",), float(default))
        self.parameters[name] = expr
        return expr

    def vector(self, name: str, default: Tuple[float, float, float, float], x: Optional[int] = None, y: Optional[int] = None) -> Any:
        expr = self.node("MaterialExpressionVectorParameter", x, y)
        set_prop(expr, ("parameter_name",), name)
        set_if_possible(expr, ("default_value",), linear_color(default))
        self.parameters[name] = expr
        return expr

    def texture(self, name: str, kind: str = "color", x: Optional[int] = None, y: Optional[int] = None) -> Any:
        expr = self.node("MaterialExpressionTextureSampleParameter2D", x, y)
        set_prop(expr, ("parameter_name",), name)
        tex = default_texture(kind)
        if tex is not None:
            set_if_possible(expr, ("texture",), tex)
        self.parameters[name] = expr
        return expr

    def cube_texture(self, name: str, x: Optional[int] = None, y: Optional[int] = None) -> Optional[Any]:
        cls = getattr(self.ue, "MaterialExpressionTextureSampleParameterCube", None)
        if cls is None:
            warn("TextureSampleParameterCube is unavailable; Cubemap parameter cannot be declared on this engine")
            return None
        expr = self.lib.create_material_expression(self.material, cls, x or self.x, y or self.y)
        set_prop(expr, ("parameter_name",), name)
        tex = default_cube_texture()
        if tex is not None:
            set_if_possible(expr, ("texture",), tex)
        self.parameters[name] = expr
        return expr

    def constant(self, value: float, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        expr = self.node("MaterialExpressionConstant", x, y)
        set_if_possible(expr, ("r",), float(value))
        return expr

    def texcoord(self, coordinate_index: int = 0, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        expr = self.node("MaterialExpressionTextureCoordinate", x, y)
        set_if_possible(expr, ("coordinate_index", "coordinate_index"), int(coordinate_index))
        return expr

    def vertex_color(self, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        return self.node("MaterialExpressionVertexColor", x, y)

    def component(self, expr: Any, channels: str, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        mask = self.node("MaterialExpressionComponentMask", x, y)
        channel_set = set(channels.upper())
        set_if_possible(mask, ("r",), "R" in channel_set)
        set_if_possible(mask, ("g",), "G" in channel_set)
        set_if_possible(mask, ("b",), "B" in channel_set)
        set_if_possible(mask, ("a",), "A" in channel_set)
        self.connect(expr, channels if len(channels) == 1 else "", mask, "Input")
        return mask

    def binary(self, class_name: str, a: Any, b: Any, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        expr = self.node(class_name, x, y)
        self.connect(a, "", expr, "A")
        self.connect(b, "", expr, "B")
        return expr

    def multiply(self, a: Any, b: Any, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        return self.binary("MaterialExpressionMultiply", a, b, x, y)

    def add(self, a: Any, b: Any, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        return self.binary("MaterialExpressionAdd", a, b, x, y)

    def subtract(self, a: Any, b: Any, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        return self.binary("MaterialExpressionSubtract", a, b, x, y)

    def divide(self, a: Any, b: Any, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        return self.binary("MaterialExpressionDivide", a, b, x, y)

    def max_node(self, a: Any, b: Any, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        expr = self.node("MaterialExpressionMax", x, y)
        self.connect(a, "", expr, "A")
        self.connect(b, "", expr, "B")
        return expr

    def lerp(self, a: Any, b: Any, alpha: Any, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        expr = self.node("MaterialExpressionLinearInterpolate", x, y)
        self.connect(a, "", expr, "A")
        self.connect(b, "", expr, "B")
        self.connect(alpha, "", expr, "Alpha")
        return expr

    def clamp01(self, value: Any, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        expr = self.node("MaterialExpressionClamp", x, y)
        self.connect(value, "", expr, "Input")
        set_if_possible(expr, ("min_default",), 0.0)
        set_if_possible(expr, ("max_default",), 1.0)
        return expr

    def uv_transform(self, prefix: str, x: int, y: int) -> Any:
        texcoord = self.texcoord(0, x, y)
        scale = self.parameters.get(f"{prefix}UVScale") or self.vector(f"{prefix}UVScale", VECTOR_DEFAULTS[f"{prefix}UVScale"], x, y + 90)
        offset = self.parameters.get(f"{prefix}UVOffset") or self.vector(f"{prefix}UVOffset", VECTOR_DEFAULTS[f"{prefix}UVOffset"], x, y + 180)
        self.parameters.get(f"{prefix}UVRotate") or self.scalar(f"{prefix}UVRotate", SCALAR_DEFAULTS[f"{prefix}UVRotate"], x, y + 270)

        scale_xy = self.component(scale, "RG", x + 220, y + 90)
        offset_xy = self.component(offset, "RG", x + 220, y + 180)
        scaled = self.multiply(texcoord, scale_xy, x + 440, y + 50)
        return self.add(scaled, offset_xy, x + 620, y + 90)

    def declare_all_parameters(self) -> None:
        y = -1200
        for name in TEXTURE_PARAMS:
            if name in ("Normal", "Normal2"):
                kind = "normal"
            elif name in ("NormalAlpha", "Normal2Alpha"):
                # Sibling alpha-mask textures default to a white mask so an unbound
                # parameter doesn't accidentally zero the specular / phong response.
                kind = "white"
            else:
                kind = "color"
            self.texture(name, kind, -2400, y)
            y += 120
        self.cube_texture("Cubemap", -2200, y)
        y += 120
        for name, default in SCALAR_DEFAULTS.items():
            if name not in self.parameters:
                self.scalar(name, default, -2000, y)
                y += 80
        for name, default in VECTOR_DEFAULTS.items():
            if name not in self.parameters:
                self.vector(name, default, -1800, y)
                y += 90

    def detail_blend(self, base_rgb: Any, x: int, y: int) -> Any:
        detail = self.parameters["Detail"]
        factor = self.parameters["DetailBlendFactor"]
        mode = self.parameters["DetailBlendMode"]

        # Mode 0, Source mod2x: base * detail * 2, blended by DetailBlendFactor.
        two = self.constant(2.0, x, y)
        mod2x = self.multiply(self.multiply(base_rgb, detail, x + 180, y), two, x + 360, y)

        # Mode 1, additive: base + detail * factor.
        additive = self.add(base_rgb, self.multiply(detail, factor, x + 180, y + 120), x + 360, y + 120)

        # Mode 2, blend: lerp(base, detail, factor).
        blend = self.lerp(base_rgb, detail, factor, x + 360, y + 240)

        # Use If nodes to select 0/1/2. Modes above 2 intentionally fall back to mod2x,
        # which is Source's common HL2-world detail mode and keeps the full parameter
        # contract live for future hand tuning.
        one = self.constant(1.0, x, y + 360)
        two_const = self.constant(2.0, x, y + 450)
        mode_lt_two = self.if_less(mode, two_const, self.if_less(mode, one, mod2x, additive, x + 600, y + 60), blend, x + 820, y + 160)
        return self.lerp(base_rgb, mode_lt_two, factor, x + 1040, y + 160)

    def if_less(self, a: Any, b: Any, less: Any, greater_equal: Any, x: Optional[int] = None, y: Optional[int] = None) -> Any:
        expr = self.node("MaterialExpressionIf", x, y)
        self.connect(a, "", expr, "A")
        self.connect(b, "", expr, "B")
        self.connect(less, "", expr, "ALessThanB")
        self.connect(greater_equal, "", expr, "AGreaterThanB")
        self.connect(greater_equal, "", expr, "AEqualsB")
        return expr

    def common_lit_outputs(self, base_rgb: Any, normal_rgb: Any, x: int, y: int) -> Dict[str, Any]:
        detailed_base = self.detail_blend(base_rgb, x, y)
        emissive_mask = self.parameters["EmissiveColor"]
        emissive_tint = self.parameters["EmissiveTint"]
        emissive_strength = self.parameters["EmissiveStrength"]
        env_tint = self.parameters["EnvmapTint"]
        phong = self.parameters["Phong"]
        phong_boost = self.parameters["PhongBoost"]

        base_alpha = self.component(self.parameters["BaseColor"], "A", x, y + 520)
        emissive = self.multiply(
            self.multiply(emissive_mask, base_alpha, x + 180, y + 520),
            self.multiply(emissive_tint, emissive_strength, x + 180, y + 650),
            x + 420,
            y + 590,
        )
        env = self.multiply(env_tint, self.constant(0.04, x + 180, y + 780), x + 420, y + 780)
        emissive_plus_env = self.add(emissive, env, x + 640, y + 660)
        specular = self.clamp01(self.multiply(phong, phong_boost, x + 180, y + 900), x + 420, y + 900)
        return {
            "base": detailed_base,
            "normal": normal_rgb,
            "emissive": emissive_plus_env,
            "specular": specular,
        }

    def build_standard(self, unlit: bool = False) -> None:
        base_uv = self.uv_transform("BaseColor", -1700, -250)
        normal_uv = self.uv_transform("Normal", -1700, 150)
        self.connect(base_uv, "", self.parameters["BaseColor"], "UVs")
        self.connect(normal_uv, "", self.parameters["Normal"], "UVs")
        base_tinted = self.multiply(self.parameters["BaseColor"], self.parameters["BaseColorTint"], -700, -250)
        outputs = self.common_lit_outputs(base_tinted, self.parameters["Normal"], -450, 60)

        if unlit:
            self.connect_property(self.add(outputs["base"], outputs["emissive"], 900, -180), "", "MP_EMISSIVE_COLOR")
            return

        self.connect_property(outputs["base"], "", "MP_BASE_COLOR")
        self.connect_property(outputs["normal"], "", "MP_NORMAL")
        self.connect_property(outputs["emissive"], "", "MP_EMISSIVE_COLOR")
        self.connect_property(outputs["specular"], "", "MP_SPECULAR")

        if self.spec.variant == "masked":
            opacity = self.multiply(self.component(self.parameters["BaseColor"], "A", 900, 200), self.parameters["OpacityScalar"], 1100, 200)
            self.connect_property(opacity, "", "MP_OPACITY_MASK")
        elif self.spec.variant in ("translucent", "decal"):
            opacity = self.multiply(self.component(self.parameters["BaseColor"], "A", 900, 200), self.parameters["OpacityScalar"], 1100, 200)
            self.connect_property(opacity, "", "MP_OPACITY")

    def build_world_blend(self) -> None:
        base_uv = self.uv_transform("BaseColor", -1700, -450)
        normal_uv = self.uv_transform("Normal", -1700, 0)
        for name in ("BaseColor", "BaseColor2"):
            self.connect(base_uv, "", self.parameters[name], "UVs")
        for name in ("Normal", "Normal2"):
            self.connect(normal_uv, "", self.parameters[name], "UVs")

        vertex_alpha = self.component(self.vertex_color(-1050, 380), "A", -850, 380)
        edge = self.component(self.parameters["BlendModulate"], "R", -1050, 520)
        soft_raw = self.component(self.parameters["BlendModulate"], "G", -1050, 640)
        soft = self.multiply(soft_raw, self.constant(0.5, -850, 640), -650, 620)
        denominator = self.max_node(self.multiply(soft, self.constant(2.0, -650, 740), -450, 700), self.constant(0.0001, -450, 820), -250, 740)
        numerator = self.subtract(vertex_alpha, self.subtract(edge, soft, -450, 500), -250, 430)
        mask = self.clamp01(self.divide(numerator, denominator, 0, 520), 200, 520)

        base_a = self.multiply(self.parameters["BaseColor"], self.parameters["BaseColorTint"], -700, -450)
        base_b = self.multiply(self.parameters["BaseColor2"], self.parameters["BaseColor2Tint"], -700, -280)
        blended_base = self.lerp(base_a, base_b, mask, 450, -360)
        blended_normal = self.lerp(self.parameters["Normal"], self.parameters["Normal2"], mask, 450, 40)

        outputs = self.common_lit_outputs(blended_base, blended_normal, 650, 160)
        self.connect_property(outputs["base"], "", "MP_BASE_COLOR")
        self.connect_property(outputs["normal"], "", "MP_NORMAL")
        self.connect_property(outputs["emissive"], "", "MP_EMISSIVE_COLOR")
        self.connect_property(outputs["specular"], "", "MP_SPECULAR")

    def build(self) -> None:
        self.declare_all_parameters()
        if self.spec.variant == "world_blend":
            self.build_world_blend()
        elif self.spec.variant == "unlit":
            self.build_standard(unlit=True)
        else:
            self.build_standard(unlit=False)

        try:
            self.lib.layout_material_expressions(self.material)
        except Exception:
            pass


def set_material_settings(material: Any, spec: MaterialSpec) -> None:
    set_prop(material, ("material_domain",), material_domain(spec.domain))
    set_prop(material, ("blend_mode",), blend_mode(spec.blend))
    set_if_possible(material, ("shading_model",), shading_model(spec.shading))
    set_if_possible(material, ("shading_models",), [shading_model(spec.shading)])
    if spec.variant == "masked":
        set_if_possible(material, ("opacity_mask_clip_value",), 0.5)
    if spec.variant == "decal":
        decal_blend = getattr(_require_unreal(), "DecalBlendMode", None)
        if decal_blend is not None:
            value = getattr(decal_blend, "DBM_TRANSLUCENT", None) or getattr(decal_blend, "DBM_Translucent", None)
            if value is not None:
                set_if_possible(material, ("decal_blend_mode",), value)


def clear_material_graph(material: Any) -> None:
    ue = _require_unreal()
    lib = ue.MaterialEditingLibrary
    try:
        expressions = list(lib.get_material_expressions(material))
    except Exception:
        expressions = []
    for expr in expressions:
        try:
            lib.delete_material_expression(material, expr)
        except Exception:
            warn(f"Could not delete existing expression {expr}; continuing")


def create_or_load_material(spec: MaterialSpec, overwrite_existing: bool) -> Tuple[Any, bool]:
    ue = _require_unreal()
    package_path = TARGET_ROOT
    asset_path = f"{package_path}/{spec.name}"
    existing = ue.EditorAssetLibrary.load_asset(asset_path)
    if existing is not None:
        if not overwrite_existing:
            log(f"Skipping existing {asset_path}")
            return existing, False
        log(f"Regenerating existing {asset_path}")
        clear_material_graph(existing)
        return existing, True

    asset_tools = ue.AssetToolsHelpers.get_asset_tools()
    factory = ue.MaterialFactoryNew()
    material = asset_tools.create_asset(spec.name, package_path, ue.Material, factory)
    if material is None:
        raise MaterialBuildError(f"Failed to create material asset {asset_path}")
    log(f"Created {asset_path}")
    return material, True


def validate_parameters(builder: GraphBuilder) -> None:
    missing = []
    for name in (*TEXTURE_PARAMS, *TEXTURE_CUBE_PARAMS, *SCALAR_DEFAULTS.keys(), *VECTOR_DEFAULTS.keys()):
        if name not in builder.parameters:
            missing.append(name)
    if missing:
        raise MaterialBuildError(f"{builder.spec.name} is missing parameter expressions: {', '.join(missing)}")


def save_material(material: Any, spec: MaterialSpec) -> None:
    ue = _require_unreal()
    lib = ue.MaterialEditingLibrary
    try:
        lib.recompile_material(material)
    except Exception as exc:
        warn(f"Recompile call failed for {spec.name}: {exc}")
    try:
        material.post_edit_change()
    except Exception:
        pass
    asset_path = f"{TARGET_ROOT}/{spec.name}"
    if not ue.EditorAssetLibrary.save_loaded_asset(material):
        if not ue.EditorAssetLibrary.save_asset(asset_path):
            raise MaterialBuildError(f"Failed to save {asset_path}")
    log(f"Saved {asset_path}.{spec.name}")


def generate_material(spec: MaterialSpec, overwrite_existing: bool) -> Optional[Any]:
    material, should_build = create_or_load_material(spec, overwrite_existing)
    if not should_build:
        return material
    set_material_settings(material, spec)
    builder = GraphBuilder(material, spec)
    builder.build()
    validate_parameters(builder)
    save_material(material, spec)
    return material


def generate_all(overwrite_existing: bool = OVERWRITE_EXISTING) -> List[Any]:
    _require_unreal()
    log(f"Generating master materials under {TARGET_ROOT} (overwrite_existing={overwrite_existing})")
    generated = []
    failures: List[str] = []
    for spec in MATERIALS:
        try:
            material = generate_material(spec, overwrite_existing)
            if material is not None:
                generated.append(material)
        except Exception as exc:
            failures.append(f"{spec.name}: {exc}")
            warn(f"{spec.name} failed: {exc}")
    if failures:
        raise MaterialBuildError("One or more master materials failed:\n" + "\n".join(failures))
    log(f"Generated or verified {len(generated)} master materials")
    return generated


def main() -> None:
    generate_all(OVERWRITE_EXISTING)


if __name__ == "__main__":
    main()