import {readFile, writeFile, mkdir, realpath, lstat} from 'node:fs/promises';
import {dirname, resolve, relative, isAbsolute} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';

const inputPath = 'config/ui-desktop-tokens.json';
const outputPaths = ['server/app/ui_desktop_tokens.hpp', 'exam/shared/ui-desktop-tokens.css'];
const snake = key => key.replace(/([A-Z])/g, '_$1').replaceAll('-', '_').toLowerCase();
const cssKey = key => snake(key).replaceAll('_', '-');
const float = value => `${Number.isInteger(value) ? value + '.0' : value}f`;
const metricGroups = ['spacing', 'radius', 'geometry', 'controls', 'typography'];
const unitless = new Set(['snapshotAspect', 'lineHeight', 'iconStroke']);
// Schema 1 is a complete contract. Adding/removing emitted keys requires a schema revision.
const schemaKeys = {
  colors: 'canvas window surface raised control hover pressed border input-border text muted disabled accent accent-hover on-accent selected selected-text focus success success-bg warning warning-bg danger danger-bg offline offline-bg info info-bg media action-text',
  spacing: 'xs sm md lg xl xxl',
  radius: 'window control card dialog badge',
  geometry: 'sidebar sidebarCompact titlebar toolbarMin statusbar contentInset contentInsetCompact tableRowMin tableHeader cardMinWidth gridGap snapshotAspect focusTargetList focusTargetListCompact dialogWidth dialogMaxWidth dialogInset studioQuestionRail studioInspector studioTitlebar studioCommandbar harness studioCanvasInset studioCanvasInsetCompact studioPromptHeight studioInstructionHeight studioPassageHeight sidebarCollapsed studioQuestionRailCompact studioInspectorCompact',
  controls: 'defaultHeight commandHeight rowHeight iconSize iconStroke focusRing borderWidth',
  typography: 'family fallback pageTitle sectionTitle body caption lineHeight weights candidateBody candidateScaleOptions studioQuestionTitle studioPrompt',
  weights: 'regular semibold medium'
};

function requireObject(value, name) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error(`${name} must be an object`);
}
function requireKeys(value, schema, name) {
  requireObject(value, name);
  if (JSON.stringify(Object.keys(value).sort()) !== JSON.stringify(schemaKeys[schema].split(' ').sort())) throw new Error(`Incomplete or unknown schema-1 keys: ${name}`);
}
function validate(tokens) {
  requireObject(tokens, 'tokens');
  if (tokens.schemaVersion !== 1) throw new Error('Unsupported token schemaVersion');
  if (!Number.isFinite(tokens.referenceDpi) || tokens.referenceDpi < 48 || tokens.referenceDpi > 768) throw new Error('Invalid referenceDpi');
  requireObject(tokens.colors, 'colors');
  for (const mode of ['light', 'dark']) requireKeys(tokens.colors[mode], 'colors', mode);
  const light = Object.keys(tokens.colors.light).sort();
  if (!light.length || JSON.stringify(light) !== JSON.stringify(Object.keys(tokens.colors.dark).sort())) throw new Error('Theme roles must match');
  for (const mode of ['light', 'dark']) for (const [role, value] of Object.entries(tokens.colors[mode])) {
    if (!/^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$/.test(role) || !/^#[a-fA-F0-9]{6}$/.test(value)) throw new Error(`Invalid color ${mode}/${role}`);
  }
  const numeric = (value, key) => {
    if (typeof value !== 'number' || !Number.isFinite(value) || value < 0 || value > 10000) throw new Error(`Invalid metric ${key}`);
  };
  const safeString = (value, key) => {
    if (typeof value !== 'string' || !/^[a-zA-Z0-9 ,._-]{1,128}$/.test(value)) throw new Error(`Unsafe font string ${key}`);
  };
  for (const group of metricGroups) {
    requireKeys(tokens[group], group, group);
    for (const [key, value] of Object.entries(tokens[group])) {
      if (!/^[a-zA-Z][a-zA-Z0-9]*$/.test(key)) throw new Error(`Invalid metric name ${group}/${key}`);
      if (group === 'typography' && ['family', 'fallback'].includes(key)) safeString(value, key);
      else if (group === 'typography' && key === 'weights') {
        requireKeys(value, 'weights', 'weights');
        for (const [weight, number] of Object.entries(value)) {
          if (!/^[a-zA-Z][a-zA-Z0-9]*$/.test(weight)) throw new Error('Invalid weight name');
          numeric(number, weight);
        }
      } else if (group === 'typography' && key === 'candidateScaleOptions') {
        if (!Array.isArray(value) || !value.length || value.some(n => !Number.isInteger(n) || n < 1 || n > 1000)) throw new Error('Invalid candidate scales');
      } else numeric(value, `${group}/${key}`);
    }
  }
  for (const required of ['family', 'fallback', 'weights', 'candidateScaleOptions']) {
    if (!(required in tokens.typography)) throw new Error(`Missing typography/${required}`);
  }
}

function render(tokens, digest) {
  const roles = Object.keys(tokens.colors.light).sort();
  const header = [`// Generated from ${inputPath}; do not edit.`, `// SHA256: ${digest}`, '#pragma once', '#include <array>', '#include <cstddef>', '#include <cstdint>', '', 'namespace nstu::ui::desktop_tokens {',
    `inline constexpr char source_sha256[] = "${digest}";`, `inline constexpr float k_reference_dpi = ${float(tokens.referenceDpi)};`,
    'enum class ColorRole : std::size_t {', ...roles.map(role => `    k_${snake(role)},`), '    count', '};',
    'using Palette = std::array<std::uint32_t, static_cast<std::size_t>(ColorRole::count)>;'];
  const css = [`/* Generated from ${inputPath}; SHA256: ${digest} */`];
  for (const mode of ['light', 'dark']) {
    header.push(`inline constexpr Palette ${mode}_palette{`, ...roles.map(role => `    0x${tokens.colors[mode][role].slice(1).toUpperCase()}, // ${role}`), '};');
    css.push(mode === 'light' ? ':root {' : ':root[data-theme="dark"] {', `  color-scheme: ${mode};`, ...roles.map(role => `  --nstu-color-${role}: ${tokens.colors[mode][role]};`), '}');
  }
  header.push('[[nodiscard]] constexpr std::uint32_t color(ColorRole role, bool dark) noexcept {', '    return (dark ? dark_palette : light_palette)[static_cast<std::size_t>(role)];', '}');
  css.push(':root {', `  --nstu-reference-dpi: ${tokens.referenceDpi};`);
  for (const group of metricGroups) {
    header.push(`namespace ${group} {`);
    for (const key of Object.keys(tokens[group]).sort()) {
      const value = tokens[group][key];
      if (typeof value === 'number') {
        header.push(`inline constexpr float k_${snake(key)} = ${float(value)};`);
        css.push(`  --nstu-${group}-${cssKey(key)}: ${value}${unitless.has(key) ? '' : 'px'};`);
      } else if (typeof value === 'string') {
        header.push(`inline constexpr char k_${snake(key)}[] = ${JSON.stringify(value)};`);
        css.push(`  --nstu-${group}-${cssKey(key)}: ${key === 'family' ? JSON.stringify(value) : value};`);
      } else if (key === 'weights') {
        for (const weight of Object.keys(value).sort()) {
          header.push(`inline constexpr float k_weights_${snake(weight)} = ${float(value[weight])};`);
          css.push(`  --nstu-typography-weight-${cssKey(weight)}: ${value[weight]};`);
        }
      } else if (key === 'candidateScaleOptions') {
        header.push(`inline constexpr std::array<unsigned, ${value.length}> k_candidate_scale_options{${value.join(', ')}};`);
        css.push(`  --nstu-typography-candidate-scale-options: ${value.join(' ')};`);
      }
    }
    header.push(`} // namespace ${group}`);
  }
  header.push('} // namespace nstu::ui::desktop_tokens', ''); css.push('}', '');
  return [header.join('\n'), css.join('\n')];
}

async function contained(root, filename, output = false) {
  const target = resolve(root, filename);
  let existing = target;
  for (;;) {
    try {
      const info = await lstat(existing);
      if (output && existing === target && (info.isSymbolicLink() || !info.isFile() || info.nlink > 1)) throw new Error(`Output symlink or file alias is not allowed: ${filename}`);
      break;
    }
    catch (error) { if (error.code !== 'ENOENT') throw error; existing = dirname(existing); }
  }
  const canonical = await realpath(existing), offset = relative(root, canonical);
  if (offset === '..' || offset.startsWith('..\\') || offset.startsWith('../') || isAbsolute(offset)) throw new Error(`Path escapes outside root: ${filename}`);
  return target;
}

async function main() {
  let root = resolve(dirname(fileURLToPath(import.meta.url)), '..'), check = false;
  const args = process.argv.slice(2);
  for (let index = 0; index < args.length; ++index) {
    if (args[index] === '--check' && !check) check = true;
    else if (args[index] === '--root' && args[index + 1]) root = resolve(args[++index]);
    else throw new Error('Usage: generate-ui-desktop-tokens.mjs [--root existing-directory] [--check]');
  }
  root = await realpath(root);
  const paths = await Promise.all([inputPath, ...outputPaths].map((filename,index) => contained(root, filename,index > 0)));
  const bytes = await readFile(paths[0]); const tokens = JSON.parse(bytes.toString('utf8')); validate(tokens);
  const generated = render(tokens, createHash('sha256').update(bytes).digest('hex'));
  if (check) {
    for (let index = 0; index < generated.length; ++index) {
      if (!(await readFile(paths[index + 1])).equals(Buffer.from(generated[index]))) throw new Error(`Stale output: ${outputPaths[index]}`);
    }
  } else {
    for (let index = 0; index < generated.length; ++index) {
      await mkdir(dirname(paths[index + 1]), {recursive: true}); await writeFile(paths[index + 1], generated[index], 'utf8');
    }
  }
  console.log(check ? 'Desktop tokens are current.' : 'Generated native/web desktop tokens.');
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
