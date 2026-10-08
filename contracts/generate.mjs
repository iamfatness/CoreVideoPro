// Intentionally small schema compiler: the supported subset is checked explicitly.
// It generates runtime validation as well as DTOs; adding unsupported schema keywords
// fails generation instead of silently weakening the wire contract.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import './observation.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const schema = JSON.parse(readFileSync(resolve(root, 'contracts/lifecycle.schema.json'), 'utf8'));
const check = process.argv.includes('--check');
const q = JSON.stringify;
const pascal = s => s[0].toUpperCase() + s.slice(1);
const identitySchema = JSON.parse(readFileSync(resolve(root, 'contracts/identity.schema.json'), 'utf8'));
const evidenceSchema = JSON.parse(readFileSync(resolve(root, 'contracts/evidence.schema.json'), 'utf8'));
const supplied = [...Object.entries(schema.$defs), ...Object.entries(identitySchema.$defs), ...Object.entries(evidenceSchema.$defs)];
if (new Set(supplied.map(([name]) => name)).size !== supplied.length) throw Error('Duplicate contract definition');
const byName = new Map(supplied), definitions = [], visited = new Set(), visiting = new Set();
const reference = rule => rule.$ref?.match(/^#\/\$defs\/(\w+)$/)?.[1];
function visit(name) {
  if (visited.has(name)) return;
  if (visiting.has(name) || !byName.has(name)) throw Error(`Cyclic/missing contract reference ${name}`);
  visiting.add(name);
  for (const rule of Object.values(byName.get(name).properties)) {
    const target = reference(rule.type === 'array' ? rule.items : rule);
    if (target) visit(target);
  }
  visiting.delete(name); visited.add(name); definitions.push([name, byName.get(name)]);
}
for (const [name] of supplied) visit(name);
const wideInteger = rule => rule.type === 'integer' && (rule.minimum < -2147483648 || rule.maximum > 2147483647);
function validateRule(rule, name) {
  if (rule.$ref) {
    if (Object.keys(rule).length !== 1 || !reference(rule) || !byName.has(reference(rule))) throw Error(`Unsupported reference ${name}`);
    return;
  }
  if (rule.type === 'array') {
    if (Object.keys(rule).some(key => !['type', 'items', 'minItems', 'maxItems'].includes(key)) ||
        !Number.isInteger(rule.minItems) || !Number.isInteger(rule.maxItems) || rule.minItems < 0 ||
        rule.maxItems < rule.minItems || rule.maxItems > 64 || !rule.items || rule.items.type === 'array')
      throw Error(`Unsupported/unbounded array ${name}`);
    validateRule(rule.items, `${name}.items`); return;
  }
  if (!['string', 'integer', 'boolean'].includes(rule.type)) throw Error(`Unsupported type ${name}`);
  for (const key of Object.keys(rule)) if (!['type', 'enum', 'minLength', 'minimum', 'maximum'].includes(key)) throw Error(`Unsupported keyword ${key}`);
  if (rule.enum && (rule.type !== 'string' || !Array.isArray(rule.enum) || rule.enum.length === 0 || rule.enum.some(value => typeof value !== 'string'))) throw Error(`Unsupported enum ${name}`);
  if ('minLength' in rule && (rule.type !== 'string' || rule.minLength !== 1)) throw Error(`Unsupported string length ${name}`);
  if (rule.type === 'integer' && (!Number.isSafeInteger(rule.minimum) || !Number.isSafeInteger(rule.maximum) || rule.minimum > rule.maximum)) throw Error(`Integer bounds must be JSON-safe integers: ${name}`);
  if (rule.type !== 'integer' && ('minimum' in rule || 'maximum' in rule)) throw Error(`Invalid numeric bounds ${name}`);
}
for (const [name, definition] of definitions) {
  if (definition.type !== 'object' || definition.additionalProperties !== true) throw Error(`Unsupported object ${name}`);
  for (const key of Object.keys(definition)) if (!['type', 'additionalProperties', 'required', 'properties'].includes(key)) throw Error(`Unsupported object keyword ${name}.${key}`);
  if (!Array.isArray(definition.required) || new Set(definition.required).size !== definition.required.length || definition.required.some(field => !(field in definition.properties))) throw Error(`Invalid required fields in ${name}`);
  for (const [field, rule] of Object.entries(definition.properties)) {
    if (rule.$ref || rule.type === 'array') { validateRule(rule, `${name}.${field}`); continue; }
    if (!['string', 'integer', 'boolean'].includes(rule.type)) throw Error(`Unsupported type ${name}.${field}`);
    for (const key of Object.keys(rule)) if (!['type', 'enum', 'minLength', 'minimum', 'maximum'].includes(key)) throw Error(`Unsupported keyword ${key}`);
    if (rule.enum && (rule.type !== 'string' || !Array.isArray(rule.enum) || rule.enum.length === 0 || rule.enum.some(value => typeof value !== 'string'))) throw Error(`Unsupported enum ${name}.${field}`);
    // Longer JSON Schema string lengths count Unicode code points, whereas the
    // native standard libraries count bytes/code units/graphemes differently.
    // Support nonempty only until a shared Unicode-length implementation exists.
    if ('minLength' in rule && (rule.type !== 'string' || rule.minLength !== 1)) throw Error(`Unsupported string length ${name}.${field}`);
    if (rule.type === 'integer' && (!Number.isSafeInteger(rule.minimum) || !Number.isSafeInteger(rule.maximum) || rule.minimum > rule.maximum)) throw Error(`Integer bounds must be JSON-safe integers: ${name}.${field}`);
    if (rule.type !== 'integer' && ('minimum' in rule || 'maximum' in rule)) throw Error(`Invalid numeric bounds ${name}.${field}`);
  }
}

const cpp = ['// Generated by contracts/generate.mjs. Do not edit.', '#pragma once', '#include "rpc/Json.h"', '#include <algorithm>', '#include <vector>', '#include <cmath>', '#include <cstdint>', '#include <optional>', '#include <string>', 'namespace corevideo::contracts {'];
const cs = ['// Generated by contracts/generate.mjs. Do not edit.', 'using System;', 'using System.Collections.Generic;', 'using System.Linq;', 'using System.Text.Json;', 'using System.Text.Json.Serialization;', 'namespace CoreVideoPro.MediaCore.Contracts;',
  'public sealed class ContractIntegerConverter : JsonConverter<int> {',
  '  public override int Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options) {',
  '    if (reader.TokenType != JsonTokenType.Number || !reader.TryGetDouble(out var value) || !double.IsFinite(value) || Math.Truncate(value) != value || value < int.MinValue || value > int.MaxValue) throw new JsonException("Expected a 32-bit integer");',
  '    return (int)value;', '  }',
  '  public override void Write(Utf8JsonWriter writer, int value, JsonSerializerOptions options) => writer.WriteNumberValue(value);', '}'];
cs.push('public sealed class ContractSafeIntegerConverter : JsonConverter<long> {',
  '  public override long Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options) {',
  '    if (reader.TokenType != JsonTokenType.Number || !reader.TryGetDouble(out var value) || !double.IsFinite(value) || Math.Truncate(value) != value || value < -9007199254740991d || value > 9007199254740991d) throw new JsonException("Expected an exact JSON-safe integer");',
  '    return (long)value;', '  }',
  '  public override void Write(Utf8JsonWriter writer, long value, JsonSerializerOptions options) {',
  '    if (value < -9007199254740991L || value > 9007199254740991L) throw new JsonException("Integer is outside JSON-safe bounds");',
  '    writer.WriteNumberValue(value);', '  }', '}');
const swift = ['// Generated by contracts/generate.mjs. Do not edit.', 'import Foundation'];
for (const [type, converter] of [['int', 'ContractIntegerConverter'], ['long', 'ContractSafeIntegerConverter']]) {
  cs.push(`public sealed class ${converter.replace('Converter', 'ArrayConverter')} : JsonConverter<${type}[]> {`,
    `  private static readonly ${converter} Element = new();`,
    `  public override ${type}[] Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options) {`,
    '    if (reader.TokenType != JsonTokenType.StartArray) throw new JsonException("Expected bounded array");',
    `    var result = new List<${type}>();`,
    '    while (reader.Read()) {', '      if (reader.TokenType == JsonTokenType.EndArray) return result.ToArray();',
    '      if (result.Count == 64) throw new JsonException("Array capacity exceeded");',
    `      result.Add(Element.Read(ref reader, typeof(${type}), options));`,
    '    }', '    throw new JsonException("Incomplete array");', '  }',
    `  public override void Write(Utf8JsonWriter writer, ${type}[] value, JsonSerializerOptions options) {`,
    '    if (value.Length > 64) throw new JsonException("Array capacity exceeded");',
    '    writer.WriteStartArray(); foreach (var item in value) Element.Write(writer, item, options); writer.WriteEndArray();', '  }', '}');
}
function wireType(rule, language) {
  if (rule.$ref) return reference(rule);
  if (rule.type === 'array') {
    const item = wireType(rule.items, language);
    return language === 'cpp' ? `std::vector<${item}>` : language === 'cs' ? `${item}[]` : `[${item}]`;
  }
  if (wideInteger(rule)) return {cpp:'std::int64_t',cs:'long',swift:'Int64'}[language];
  return {cpp:{string:'std::string',integer:'int',boolean:'bool'},cs:{string:'string',integer:'int',boolean:'bool'},swift:{string:'String',integer:'Int',boolean:'Bool'}}[language][rule.type];
}
function cppCheck(rule, value) {
  const v = `(${value})`;
  if (rule.$ref) return `validate${reference(rule)}(${value})`;
  if (rule.type === 'array') return `${v}.isArray() && ${v}.asArray().size() >= ${rule.minItems} && ${v}.asArray().size() <= ${rule.maxItems} && std::all_of(${v}.asArray().begin(), ${v}.asArray().end(), [](const auto& item) { return ${cppCheck(rule.items, 'item')}; })`;
  const checks = [`${v}.${{string:'isString',integer:'isNumber',boolean:'isBool'}[rule.type]}()`];
  if (rule.type === 'integer') checks.push(`std::floor(${v}.asNumber()) == ${v}.asNumber()`, `${v}.asNumber() >= ${rule.minimum}`, `${v}.asNumber() <= ${rule.maximum}`);
  if (rule.minLength) checks.push(`!${v}.asString().empty()`);
  if (rule.enum) checks.push(`(${rule.enum.map(x => `${v}.asString() == ${q(x)}`).join(' || ')})`);
  return checks.join(' && ');
}
function csCheck(rule, value) {
  if (rule.$ref) return `${reference(rule)}Contract.Validate(${value})`;
  if (rule.type === 'array') return `${value}.ValueKind == JsonValueKind.Array && ${value}.GetArrayLength() >= ${rule.minItems} && ${value}.GetArrayLength() <= ${rule.maxItems} && ${value}.EnumerateArray().All(item => ${csCheck(rule.items, 'item')})`;
  const checks = [rule.type === 'boolean' ? `(${value}.ValueKind == JsonValueKind.True || ${value}.ValueKind == JsonValueKind.False)` : `${value}.ValueKind == JsonValueKind.${rule.type === 'string' ? 'String' : 'Number'}`];
  if (rule.type === 'integer') checks.push(`${value}.TryGetDouble(out var ${value}Number)`, `double.IsFinite(${value}Number)`, `Math.Truncate(${value}Number) == ${value}Number`, `${value}Number >= ${rule.minimum}`, `${value}Number <= ${rule.maximum}`);
  if (rule.minLength) checks.push(`${value}.GetString()!.Length >= 1`);
  if (rule.enum) checks.push(`(${rule.enum.map(x => `${value}.GetString() == ${q(x)}`).join(' || ')})`);
  return checks.join(' && ');
}
function swiftCheck(rule, value, indent, depth = 0) {
  const parsed = `parsed${depth}`;
  if (rule.$ref) {
    swift.push(`${indent}guard let ${parsed} = ${value} as? [String: Any], validate${reference(rule)}(${parsed}) else { return false }`);
  } else if (rule.type === 'array') {
    swift.push(`${indent}guard let ${parsed} = ${value} as? [Any], ${parsed}.count >= ${rule.minItems}, ${parsed}.count <= ${rule.maxItems} else { return false }`, `${indent}for item in ${parsed} {`);
    swiftCheck(rule.items, 'item', `${indent}  `, depth + 1); swift.push(`${indent}}`);
  } else if (rule.type === 'string') {
    swift.push(`${indent}guard let ${parsed} = ${value} as? String else { return false }`);
    if (rule.minLength) swift.push(`${indent}if ${parsed}.isEmpty { return false }`);
    if (rule.enum) swift.push(`${indent}if !${q(rule.enum)}.contains(${parsed}) { return false }`);
  } else {
    swift.push(`${indent}guard let ${parsed} = ${value} as? NSNumber else { return false }`,
      `${indent}if CFGetTypeID(${parsed}) ${rule.type === 'boolean' ? '!=' : '=='} CFBooleanGetTypeID() { return false }`);
    if (rule.type === 'integer') swift.push(`${indent}if ${parsed}.doubleValue.rounded() != ${parsed}.doubleValue || ${parsed}.doubleValue < ${rule.minimum} || ${parsed}.doubleValue > ${rule.maximum} { return false }`);
  }
}

for (const [name, def] of definitions) {
  const fields = Object.entries(def.properties);
  const required = field => def.required.includes(field);
  cpp.push(`struct ${name} {`);
  cs.push(`public sealed record ${name} {`);
  swift.push(`struct ${name}: Codable {`);
  for (const [field, rule] of fields) {
    const cppType = wireType(rule, 'cpp'), csType = wireType(rule, 'cs'), swiftType = wireType(rule, 'swift');
    cpp.push(`  ${required(field) ? cppType : `std::optional<${cppType}>`} ${field}{};`);
    if (!required(field)) cs.push('  [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]');
    if (rule.type === 'integer') cs.push(`  [JsonConverter(typeof(${wideInteger(rule) ? 'ContractSafeIntegerConverter' : 'ContractIntegerConverter'}))]`);
    if (rule.type === 'array' && rule.items.type === 'integer') cs.push(`  [JsonConverter(typeof(${wideInteger(rule.items) ? 'ContractSafeIntegerArrayConverter' : 'ContractIntegerArrayConverter'}))]`);
    cs.push(`  [JsonPropertyName(${q(field)})] public ${required(field) ? 'required ' : ''}${csType}${required(field) ? '' : '?'} ${pascal(field)} { get; init; }`);
    swift.push(`  var ${field}: ${swiftType}${required(field) ? '' : '?'}${required(field) ? '' : ' = nil'}`);
  }
  cpp.push('};'); cs.push('}'); swift.push('}');

  cpp.push(`inline bool validate${name}(const rpc::Json& value) {`, '  if (!value.isObject()) return false;');
  cs.push(`public static class ${name}Contract {`, '  public static bool Validate(JsonElement value) {', '    if (value.ValueKind != JsonValueKind.Object) return false;');
  swift.push(`func validate${name}(_ value: [String: Any]) -> Bool {`);
  for (const [field, rule] of fields) {

    if (rule.$ref || rule.type === 'array') {
      cpp.push(`  const auto* ${field} = value.get(${q(field)});`, `  if (${required(field) ? `!${field} || ` : `${field} && `}!(${cppCheck(rule, `*${field}`)})) return false;`);
      cs.push(`    var has${pascal(field)} = value.TryGetProperty(${q(field)}, out var ${field});`, `    if (${required(field) ? `!has${pascal(field)} || ` : `has${pascal(field)} && `}!(${csCheck(rule, field)})) return false;`);
      swift.push(`  if let raw = value[${q(field)}] {`); swiftCheck(rule, 'raw', '    ');
      swift.push(`  }${required(field) ? ' else { return false }' : ''}`); continue;
    }

    cpp.push(`  const auto* ${field} = value.get(${q(field)});`);
    const cc = [`${field}->${{string:'isString',integer:'isNumber',boolean:'isBool'}[rule.type]}()`];
    if (rule.type === 'integer') cc.push(`std::floor(${field}->asNumber()) == ${field}->asNumber()`, `${field}->asNumber() >= ${rule.minimum}`, `${field}->asNumber() <= ${rule.maximum}`);
    if (rule.minLength) cc.push(`${field}->asString().size() >= ${rule.minLength}`);
    if (rule.enum) cc.push(`(${rule.enum.map(x => `${field}->asString() == ${q(x)}`).join(' || ')})`);
    cpp.push(`  if (${required(field) ? `!${field} || ` : `${field} && `}!(${cc.join(' && ')})) return false;`);

    cs.push(`    var has${pascal(field)} = value.TryGetProperty(${q(field)}, out var ${field});`);
    const csc = [`${field}.ValueKind == JsonValueKind.${{string:'String',integer:'Number',boolean:'True'}[rule.type]}`];
    if (rule.type === 'boolean') csc[0] = `(${field}.ValueKind == JsonValueKind.True || ${field}.ValueKind == JsonValueKind.False)`;
    if (rule.type === 'integer') csc.push(`${field}.TryGetDouble(out var ${field}Number)`, `double.IsFinite(${field}Number)`, `Math.Truncate(${field}Number) == ${field}Number`, `${field}Number >= ${rule.minimum}`, `${field}Number <= ${rule.maximum}`);
    if (rule.minLength) csc.push(`${field}.GetString()!.Length >= ${rule.minLength}`);
    if (rule.enum) csc.push(`(${rule.enum.map(x => `${field}.GetString() == ${q(x)}`).join(' || ')})`);
    cs.push(`    if (${required(field) ? `!has${pascal(field)} || ` : `has${pascal(field)} && `}!(${csc.join(' && ')})) return false;`);

    swift.push(`  if let raw = value[${q(field)}] {`);
    if (rule.type === 'string') {
      swift.push('    guard let parsed = raw as? String else { return false }');
      if (rule.minLength) swift.push(`    if parsed.isEmpty { return false }`);
      if (rule.enum) swift.push(`    if !${q(rule.enum)}.contains(parsed) { return false }`);
      if (!rule.enum && !rule.minLength) swift.push('    _ = parsed');
    } else {
      swift.push('    guard let parsed = raw as? NSNumber else { return false }');
      swift.push(`    if ${rule.type === 'boolean' ? 'CFGetTypeID(parsed) != CFBooleanGetTypeID()' : 'CFGetTypeID(parsed) == CFBooleanGetTypeID()'} { return false }`);
      if (rule.type === 'integer') swift.push(`    if parsed.doubleValue.rounded() != parsed.doubleValue || parsed.doubleValue < ${rule.minimum} || parsed.doubleValue > ${rule.maximum} { return false }`);
    }
    swift.push(`  }${required(field) ? ' else { return false }' : ''}`);
  }
  cpp.push('  return true;', '}'); cs.push('    return true;', '  }', '}'); swift.push('  return true;', '}');
  cpp.push(`inline rpc::Json toJson(const ${name}& value) {`, '  rpc::Json::Object result;');
  for (const [field, rule] of fields) {
    const access = `${required(field) ? '' : '*'}value.${field}`;
    if (rule.type === 'array') {
      cpp.push(`  ${required(field) ? '' : `if (value.${field}) `}{`, '    rpc::Json::Array items;',
        `    for (const auto& item : ${access}) items.emplace_back(${rule.items.$ref ? 'toJson(item)' : wideInteger(rule.items) ? 'static_cast<double>(item)' : 'item'});`,
        `    result.emplace(${q(field)}, std::move(items));`, '  }'); continue;
    }
    if (rule.$ref) { cpp.push(`  ${required(field) ? '' : `if (value.${field}) `}result.emplace(${q(field)}, toJson(${access}));`); continue; }
    cpp.push(`  ${required(field) ? '' : `if (value.${field}) `}result.emplace(${q(field)}, ${wideInteger(rule) ? `static_cast<double>(${access})` : access});`);
  }
  cpp.push('  return result;', '}');
}
cpp.push('} // namespace corevideo::contracts');
swift.splice(2, 0, 'import CoreFoundation');
let stale = false;
for (const [path, lines] of [
  ['native/src/contracts/Lifecycle.h', cpp],
  ['native-shell/CoreVideoPro.MediaCore/Contracts/Lifecycle.cs', cs],
  ['mac-shell/Sources/CoreVideoProShell/Lifecycle.generated.swift', swift]
]) {
  const target = resolve(root, path);
  const content = lines.join('\n') + '\n';
  if (check) {
    let existing = ''; try { existing = readFileSync(target, 'utf8'); } catch {}
    if (existing.replaceAll('\r\n', '\n') !== content) { console.error(`Stale generated contract: ${path}`); stale = true; }
  } else { mkdirSync(dirname(target), {recursive:true}); writeFileSync(target, content); }
}
if (stale) process.exitCode = 1;
