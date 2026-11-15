import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import urllib.request
import zipfile


ROOT = Path(__file__).resolve().parent.parent
MATHLIB_SHA256 = "4a0772103fd7ef832f8c2fb3ec7fb6bf5b3bebbd41ecafc476671d11eca28a06"
RESOURCE = re.compile(
    r"\b(RWTexture2D|Texture2D)\s*<\s*([^>]+?)\s*>\s+(\w+)\s*:\s*"
    r"register\s*\(\s*([tu])(\d+)\s*,\s*space(\d+)\s*\)\s*;"
)


def closing(source, start):
    pairs = {"(": ")", "[": "]", "{": "}"}
    stack = [pairs[source[start]]]
    for position in range(start + 1, len(source)):
        token = source[position]
        if token in pairs:
            stack.append(pairs[token])
        elif token in ")]}":
            if not stack or token != stack.pop():
                raise ValueError("Unbalanced shader expression")
            if not stack:
                return position
    raise ValueError("Unclosed shader expression")


def strip_comments(source):
    return re.sub(r"/\*.*?\*/|//[^\n]*", lambda match: "\n" * match[0].count("\n"), source, flags=re.S)


def arguments(source):
    result = []
    start = 0
    position = 0
    while position < len(source):
        if source[position] in "([{":
            position = closing(source, position)
        elif source[position] == ",":
            result.append(source[start:position].strip())
            start = position + 1
        position += 1
    result.append(source[start:].strip())
    return result


def inline_texture_helpers(source):
    resource_types = {resource[3]: resource[2] for resource in RESOURCE.finditer(source)}
    helpers = {}
    removals = []
    for match in re.finditer(r"\bvoid\s+(\w+)\s*\(", source):
        parameter_end = closing(source, match.end() - 1)
        parameters = arguments(source[match.end():parameter_end])
        if not any("Texture2D<" in parameter for parameter in parameters):
            continue
        opening = source.index("{", parameter_end)
        end = closing(source, opening)
        helpers.setdefault(match[1], []).append((parameters, source[opening + 1:end]))
        removals.append((match.start(), end + 1))
    for start, end in reversed(removals):
        source = source[:start] + source[end:]
    serial = 0
    for name, overloads in helpers.items():
        call = re.compile(r"\b" + re.escape(name) + r"\s*\(")
        while match := call.search(source):
            end = closing(source, match.end() - 1)
            values = arguments(source[match.end():end])
            compatible = []
            for parameters, body in overloads:
                if len(parameters) != len(values):
                    continue
                if all("Texture2D<" not in parameter or
                       re.search(r"Texture2D<([^>]+)>", parameter)[1] == resource_types.get(value)
                       for parameter, value in zip(parameters, values, strict=True)):
                    compatible.append((parameters, body))
            if len(compatible) != 1:
                raise ValueError(f"Unsupported texture helper overload: {name}")
            parameters, body = compatible[0]
            aliases = {}
            locals_code = []
            for parameter, value in zip(parameters, values, strict=True):
                parameter_name = parameter.split()[-1]
                if "Texture2D<" in parameter or parameter.startswith("out "):
                    aliases[parameter_name] = value
                else:
                    aliases[parameter_name] = f"mcdrInline{serial}_{parameter_name}"
                    locals_code.append(parameter[:parameter.rfind(parameter_name)] + aliases[parameter_name] + f" = {value};")
            body = re.sub(r"\b(" + "|".join(map(re.escape, aliases)) + r")\b", lambda token: aliases[token[0]], body)
            source = source[:match.start()] + "{" + "\n".join(locals_code) + body + "}" + source[end + 1:]
            serial += 1
    return source


def instrument(source, support):
    source = inline_texture_helpers(strip_comments(source))
    resources = list(RESOURCE.finditer(source))
    if not resources or len(resources) > 32:
        raise ValueError("Unsupported NRD resource declarations")
    inputs = [resource for resource in resources if resource[1] == "Texture2D"]
    outputs = [resource for resource in resources if resource[1] == "RWTexture2D"]
    if len(outputs) > 8 or any(int(resource[5]) != index for index, resource in enumerate(inputs)):
        raise ValueError("Unsupported NRD input registers")
    if any(int(resource[5]) != index for index, resource in enumerate(outputs)):
        raise ValueError("Unsupported NRD output registers")
    declarations = {resource[3]: resource for resource in resources}
    counts = {name: {} for name in declarations}
    body_start = resources[-1].end()
    prefix = source[:body_start]
    body = source[body_start:]
    expression = re.compile(r"\b(" + "|".join(map(re.escape, declarations)) + r")\b\s*")

    def transform(code):
        result = []
        cursor = 0
        while match := expression.search(code, cursor):
            result.append(code[cursor:match.start()])
            name = match[1]
            resource = declarations[name]
            position = match.end()
            if code[position] == ".":
                method = re.match(r"\.\s*(\w+)\s*\(", code[position:])
                if not method or resource[1] != "Texture2D" or method[1] not in (
                    "Load", "SampleLevel", "GatherRed", "GatherGreen", "GatherBlue", "GatherAlpha"
                ):
                    raise ValueError(f"Unsupported NRD texture operation: {name}: {code[position:position + 80]}")
                opening = position + method.end() - 1
                end = closing(code, opening)
                operation = method[1]
                result.append(f"Mcdr_{name}_{operation}({transform(code[opening + 1:end])})")
                cursor = end + 1
            elif code[position] == "[":
                end = closing(code, position)
                coordinate = transform(code[position + 1:end])
                if resource[1] == "Texture2D":
                    operation = "Read"
                    result.append(f"Mcdr_{name}_Read({coordinate})")
                    cursor = end + 1
                else:
                    assignment = re.match(r"\s*=\s*(?!=)", code[end + 1:])
                    if not assignment:
                        operation = "Read"
                        result.append(f"Mcdr_{name}_Read({coordinate})")
                        cursor = end + 1
                    else:
                        value_start = end + 1 + assignment.end()
                        value_end = value_start
                        while code[value_end] != ";":
                            if code[value_end] in "([{":
                                value_end = closing(code, value_end)
                            value_end += 1
                        operation = "Write"
                        result.append(f"Mcdr_{name}_Write({coordinate}, {transform(code[value_start:value_end])});")
                        cursor = value_end + 1
            else:
                raise ValueError(f"Untracked NRD resource use: {name}: {code[position:position + 80]}")
            counts[name][operation] = counts[name].get(operation, 0) + 1
        result.append(code[cursor:])
        return "".join(result)

    body = transform(body)
    entry = re.search(r"\[\s*numthreads\s*\(([^)]+)\)\s*\]\s*void\s+main\s*\(", body)
    if not entry:
        raise ValueError("NRD compute entry unavailable")
    parameter_start = entry.end() - 1
    parameter_end = closing(body, parameter_start)
    parameters = body[parameter_start + 1:parameter_end]
    plain_parameters = re.sub(r"\s*:\s*SV_\w+", "", parameters)
    names = [parameter.split()[-1] for parameter in plain_parameters.split(",")]
    if "threadIndex" not in names:
        raise ValueError("NRD group index unavailable")
    body = (body[:entry.start()] + "void McdrNrdBody(" + plain_parameters + ")"
            + body[parameter_end + 1:])
    wrappers = []
    for resource in inputs:
        data_type, name, binding = resource[2], resource[3], int(resource[5])
        used = counts[name]
        if "Read" in used:
            wrappers.append(f"{data_type} Mcdr_{name}_Read(int2 position) {{ McdrInclude({binding}, position, false); return {name}[position]; }}")
        if "Load" in used:
            wrappers.append(f"{data_type} Mcdr_{name}_Load(int3 position, int2 offset = 0) {{ McdrInclude({binding}, position.xy + offset, false); return {name}.Load(position, offset); }}")
        if "SampleLevel" in used:
            wrappers.append(f"{data_type} Mcdr_{name}_SampleLevel(SamplerState samplerValue, float2 coordinate, float level, int2 offset = 0) {{ McdrSample({binding}, coordinate, offset); return {name}.SampleLevel(samplerValue, coordinate, level, offset); }}")
        for channel in ("Red", "Green", "Blue", "Alpha"):
            if "Gather" + channel in used:
                scalar = re.sub(r"[1-4]$", "", data_type)
                wrappers.append(f"{scalar}4 Mcdr_{name}_Gather{channel}(SamplerState samplerValue, float2 coordinate, int2 offset = 0) {{ McdrSample({binding}, coordinate, offset); return {name}.Gather{channel}(samplerValue, coordinate, offset); }}")
    for resource in outputs:
        data_type, name, binding = resource[2], resource[3], int(resource[5])
        wrappers.append(f"void Mcdr_{name}_Write(int2 position, {data_type} value) {{ McdrWrite({len(inputs) + binding}, {binding}, position); {name}[position] = value; }}")
        if "Read" in counts[name]:
            wrappers.append(f"{data_type} Mcdr_{name}_Read(int2 position) {{ McdrInclude({len(inputs) + binding}, position, false); return {name}[position]; }}")
    wrapper_entry = ("\n[numthreads(" + entry[1] + ")]\nvoid main(" + parameters + ") {\n"
                     "    McdrBegin(threadIndex);\n    McdrNrdBody(" + ", ".join(names) + ");\n"
                     f"    McdrEnd(threadIndex, {len(inputs)}, {len(outputs)});\n}}\n")
    result = prefix + "\n" + support + "\n" + "\n".join(wrappers) + "\n" + body + wrapper_entry
    return result, {"inputs": len(inputs), "outputs": len(outputs), "accesses": counts}


def permutations():
    yield "REBLUR_ClassifyTiles.cs.hlsl", []
    for signal in ("NRD_SIGNAL_DIFF", "NRD_SIGNAL_SPEC"):
        common = [("NRD_SIGNAL", signal), ("NRD_MODE", "NRD_MODE_RADIANCE")]
        for shader in ("PrePass", "TemporalAccumulation", "HistoryFix", "Blur", "TemporalStabilization", "SplitScreen"):
            yield f"REBLUR_{shader}.cs.hlsl", common
        for radius in ("0", "1"):
            yield "REBLUR_HitDistReconstruction.cs.hlsl", common + [("MODE_5X5", radius)]
            yield "REBLUR_PostBlur.cs.hlsl", common + [("TEMPORAL_STABILIZATION", radius)]


def run(command):
    result = subprocess.run(list(map(str, command)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace", timeout=120)
    if result.returncode or re.search(r"\b(?:fatal )?error:", result.stdout):
        raise RuntimeError(result.stdout)


def build(arguments):
    shader_root = ROOT / "external/nrd/Shaders"
    compiler = arguments.compiler or ROOT / "vcpkg_installed/x64-windows/x64-windows/tools/directx-dxc/dxc.exe"
    output = arguments.output or ROOT / "build/nrd-dependencies.h"
    output.parent.mkdir(parents=True, exist_ok=True)
    mathlib = arguments.mathlib
    if not mathlib:
        mathlib = ROOT / "build/nrd-mathlib/MathLib-11"
        if not (mathlib / "ml.hlsli").is_file():
            mathlib.parent.mkdir(parents=True, exist_ok=True)
            archive = mathlib.parent / "v11.zip"
            urllib.request.urlretrieve("https://codeload.github.com/NVIDIA-RTX/MathLib/zip/refs/tags/v11", archive)
            if hashlib.sha256(archive.read_bytes()).hexdigest() != MATHLIB_SHA256:
                raise RuntimeError("MathLib v11 archive hash mismatch")
            with zipfile.ZipFile(archive) as compressed:
                compressed.extractall(mathlib.parent)
            archive.unlink()
    support_file = ROOT / "include/shaders/nrd_dependencies.hlsli"
    update_file = ROOT / "include/shaders/nrd_dependency_update.hlsl"
    sources = [Path(__file__), support_file, update_file, compiler]
    sources.extend(sorted(shader_root.glob("*.hlsl")))
    sources.extend(sorted(shader_root.glob("*.hlsli")))
    sources.extend(sorted(mathlib.glob("*.hlsli")))
    digest = hashlib.sha256()
    for path in sources:
        digest.update(path.name.encode())
        digest.update(path.read_bytes())
    fingerprint = digest.hexdigest()
    receipt = output.with_suffix(".json")
    if output.is_file() and receipt.is_file() and json.loads(receipt.read_text())["fingerprint"] == fingerprint:
        return
    support = support_file.read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="_tmp_nrd_shader_", dir=ROOT.parent) as directory:
        work = Path(directory)
        configuration = {
            "NRD_NORMAL_ENCODING": 2, "NRD_ROUGHNESS_ENCODING": 1,
            "NRD_SUPPORTS_VIEWPORT_OFFSET": 0, "NRD_SUPPORTS_CHECKERBOARD": 1,
            "NRD_SUPPORTS_HISTORY_CONFIDENCE": 1, "NRD_SUPPORTS_DISOCCLUSION_THRESHOLD_MIX": 1,
            "NRD_SUPPORTS_ANTIFIREFLY": 1, "NRD_SUPPORTS_QUAD_INTRINSICS": 1,
            "REBLUR_PERFORMANCE_MODE": 0,
        }
        (work / "NRDConfig.hlsli").write_text("".join(f"#define {name} {value}\n" for name, value in configuration.items()), encoding="utf-8")

        def compile_variant(item):
            variant, (filename, defines) = item
            identifier = filename + "".join(f"|{name}={value}" for name, value in defines)
            preprocessed = work / f"{variant}.pre.hlsl"
            flags = ["-T", "cs_6_9", "-E", "main", "-I", mathlib, "-I", work,
                     "-D", "NRD_INTERNAL", "-D", "COMPILER_DXC=1"]
            for name, value in defines:
                flags.extend(["-D", f"{name}={value}"])
            run([compiler, *flags, "-P", "-Fi", preprocessed, shader_root / filename])
            transformed, metadata = instrument(preprocessed.read_text(encoding="utf-8-sig"), support)
            shader = work / f"{variant}.hlsl"
            shader.write_text(transformed, encoding="utf-8")
            bytecode = work / f"{variant}.dxil"
            run([compiler, "-T", "cs_6_9", "-E", "main", "-WX", "-O3", "-Fo", bytecode, shader])
            metadata.update(identifier=identifier, dxil_sha256=hashlib.sha256(bytecode.read_bytes()).hexdigest())
            return bytecode.read_bytes(), metadata

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
            variants = list(executor.map(compile_variant, enumerate(permutations())))
        update_bytecode = work / "update.dxil"
        run([compiler, "-T", "cs_6_9", "-E", "main", "-WX", "-O3", "-Fo", update_bytecode, update_file])
        update_binary = update_bytecode.read_bytes()
    lines = ["struct McdrNrdTrackedShader { std::string_view identifier; const void* bytecode; size_t size; uint32_t inputs; uint32_t outputs; };\n"]
    lines.append("constexpr unsigned char kMcdrNrdDependencyUpdate[] = {\n")
    for offset in range(0, len(update_binary), 32):
        lines.append(",".join(map(str, update_binary[offset:offset + 32])) + ",\n")
    lines.append("};\n")
    for variant, (bytecode, metadata) in enumerate(variants):
        lines.append(f"constexpr unsigned char kMcdrNrdTracked{variant}[] = {{\n")
        for offset in range(0, len(bytecode), 32):
            lines.append(",".join(map(str, bytecode[offset:offset + 32])) + ",\n")
        lines.append("};\n")
    lines.append("constexpr McdrNrdTrackedShader kMcdrNrdTrackedShaders[] = {\n")
    for variant, (bytecode, metadata) in enumerate(variants):
        lines.append(f'{{"{metadata["identifier"]}", kMcdrNrdTracked{variant}, sizeof(kMcdrNrdTracked{variant}), {metadata["inputs"]}, {metadata["outputs"]}}},\n')
    lines.append("};\n")
    output.write_text("".join(lines), encoding="utf-8")
    receipt.write_text(json.dumps({"fingerprint": fingerprint, "variants": [metadata for bytecode, metadata in variants]}, indent=2), encoding="utf-8")
    print(f"nrd dependencies: {len(variants)} shaders compiled")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--mathlib", type=Path)
    build(parser.parse_args())
