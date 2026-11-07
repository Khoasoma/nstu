import assert from 'node:assert/strict';
import {mkdtemp, mkdir, readFile, writeFile, rm, symlink, link, readdir} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import test from 'node:test';
import {fileURLToPath} from 'node:url';

const repo = resolve(fileURLToPath(new URL('../', import.meta.url)));
const generator = join(repo, 'scripts/generate-ui-desktop-tokens.mjs');
const canonical = await readFile(join(repo, 'config/ui-desktop-tokens.json'));
const tokens = JSON.parse(canonical);
const outputs = ['server/app/ui_desktop_tokens.hpp', 'exam/shared/ui-desktop-tokens.css'];
const digest = bytes => createHash('sha256').update(bytes).digest('hex');
const run = (root, ...args) => spawnSync(process.execPath, [generator, '--root', root, ...args], {encoding: 'utf8'});
async function fixture() {
  const root = await mkdtemp(join(tmpdir(), 'nstu-token-'));
  await mkdir(join(root, 'config'));
  await writeFile(join(root, 'config/ui-desktop-tokens.json'), canonical);
  return root;
}
async function rendered(root) { return Promise.all(outputs.map(p => readFile(join(root,p)))); }

test('Git preserves the approved input bytes used by generated source digests', () => {
  const input = 'config/ui-desktop-tokens.json';
  const filtered = spawnSync('git',['hash-object','--path='+input,input],{cwd:repo,encoding:'utf8'});
  const raw = spawnSync('git',['hash-object','--no-filters',input],{cwd:repo,encoding:'utf8'});
  assert.equal(filtered.status,0,filtered.stderr);
  assert.equal(raw.status,0,raw.stderr);
  assert.equal(filtered.stdout.trim(),raw.stdout.trim(),'Git normalization must not change approved source bytes');
});
const nativeName = key => key.replace(/([A-Z])/g,'_$1').replaceAll('-','_').toLowerCase();
const cssName = key => nativeName(key).replaceAll('_','-');
const declarations = block => Object.fromEntries([...block.matchAll(/^  ([\w-]+): (.*);$/gm)].map(m=>[m[1],m[2]]));

function assertParity(header, css, source = tokens, bytes = canonical) {
  assert(header.includes(digest(bytes)) && css.includes(digest(bytes)));
  assert.equal(Number(header.match(/k_reference_dpi = ([\d.]+)f;/)[1]), source.referenceDpi);
  const roles = Object.keys(source.colors.light).sort();
  const enumBlock = header.match(/enum class ColorRole : std::size_t \{([\s\S]*?)\};/)[1];
  assert.deepEqual([...enumBlock.matchAll(/^    (\w+),?$/gm)].map(m=>m[1]), [...roles.map(r=>'k_'+nativeName(r)), 'count']);
  const blocks = [...css.matchAll(/(:root(?:\[data-theme="dark"\])?) \{([^}]+)\}/g)];
  assert.deepEqual(blocks.map(m=>m[1]), [':root', ':root[data-theme="dark"]', ':root']);
  for (const [index, mode] of ['light','dark'].entries()) {
    const nativeBlock = header.match(new RegExp(`Palette ${mode}_palette\\{([\\s\\S]*?)\\};`))[1];
    assert.deepEqual([...nativeBlock.matchAll(/0x([A-F0-9]{6}), \/\/ ([\w-]+)/g)].map(m=>[m[2],m[1]]),
      roles.map(role=>[role,source.colors[mode][role].slice(1).toUpperCase()]));
    assert.deepEqual(declarations(blocks[index][2]), Object.fromEntries([
      ['color-scheme',mode], ...roles.map(role=>[`--nstu-color-${role}`,source.colors[mode][role]])]));
  }
  const expectedCss = {'--nstu-reference-dpi':String(source.referenceDpi)};
  for (const group of ['spacing','radius','geometry','controls','typography']) {
    const nativeBlock = header.match(new RegExp(`namespace ${group} \\{([\\s\\S]*?)\\} // namespace ${group}`))[1];
    const nativeNumbers = Object.fromEntries([...nativeBlock.matchAll(/float (k_\w+) = ([\d.]+)f;/g)].map(m=>[m[1],Number(m[2])]));
    const nativeStrings = Object.fromEntries([...nativeBlock.matchAll(/char (k_\w+)\[\] = ("[^"]*");/g)].map(m=>[m[1],JSON.parse(m[2])]));
    const expectedNumbers = {}, expectedStrings = {};
    for (const [key,value] of Object.entries(source[group])) {
      const cssProperty = `--nstu-${group}-${cssName(key)}`;
      if (typeof value === 'number') {
        expectedNumbers['k_'+nativeName(key)] = value;
        expectedCss[cssProperty] = String(value) + (['snapshotAspect','lineHeight','iconStroke'].includes(key) ? '' : 'px');
      } else if (typeof value === 'string') {
        expectedStrings['k_'+nativeName(key)] = value;
        expectedCss[cssProperty] = key === 'family' ? JSON.stringify(value) : value;
      } else if (key === 'weights') {
        for (const [weight,number] of Object.entries(value)) {
          expectedNumbers['k_weights_'+nativeName(weight)] = number;
          expectedCss['--nstu-typography-weight-'+cssName(weight)] = String(number);
        }
      } else if (key === 'candidateScaleOptions') {
        const match = nativeBlock.match(/std::array<unsigned, (\d+)> k_candidate_scale_options\{([\d, ]+)\};/);
        assert.equal(Number(match[1]),value.length);
        assert.deepEqual(match[2].split(',').map(Number),value);
        expectedCss[cssProperty] = value.join(' ');
      }
    }
    assert.deepEqual(nativeNumbers,expectedNumbers,group+' numeric parity');
    assert.deepEqual(nativeStrings,expectedStrings,group+' font parity');
  }
  assert.deepEqual(declarations(blocks[2][2]),expectedCss);
}

test('repository outputs, all palette roles, metrics and source digest agree with JSON', async () => {
  assert.equal(run(repo, '--check').status, 0);
  const [header, css] = (await rendered(repo)).map(b => b.toString());
  assert(header.includes(digest(canonical)) && css.includes(digest(canonical)));
  for (const [mode, palette] of Object.entries(tokens.colors)) {
    const nativeBlock=header.match(new RegExp(`Palette ${mode}_palette\\{([\\s\\S]*?)\\};`))[1];
    assert.deepEqual([...nativeBlock.matchAll(/0x([A-F0-9]{6})/g)].map(match=>match[1]),
      Object.keys(palette).sort().map(role=>palette[role].slice(1).toUpperCase()));
    for (const [role, value] of Object.entries(palette)) {
      assert(header.includes('0x' + value.slice(1).toUpperCase()), `${mode}/${role}`);
      assert(css.includes(`--nstu-color-${role}: ${value};`), `${mode}/${role}`);
    }
  }
  for (const group of ['spacing','radius','geometry','controls','typography']) {
    for (const [key, value] of Object.entries(tokens[group])) {
      if (typeof value !== 'number') continue;
      assert(header.includes(`k_${key.replace(/([A-Z])/g,'_$1').toLowerCase()} = ${Number.isInteger(value) ? value+'.0' : value}f;`), `${group}/${key}`);
      assert(css.includes(`--nstu-${group}-${key.replace(/([A-Z])/g,'-$1').toLowerCase()}: ${value}`));
    }
  }
  assert(header.includes('k_family[] = "Segoe UI"'));
  assert(header.includes('k_weights_semibold = 600.0f'));
  assert(header.includes('k_candidate_scale_options{100, 110, 120, 130}'));
  assertParity(header,css);
});

test('parity oracle rejects reordered roles, wrong theme, units, weights, fonts, scales and DPI', async () => {
  const [header,css] = (await rendered(repo)).map(b=>b.toString());
  const faults = [
    [header.replace('    k_accent,\n    k_accent_hover,','    k_accent_hover,\n    k_accent,'),css],
    [header,css.replace('--nstu-color-text: '+tokens.colors.dark.text,'--nstu-color-text: '+tokens.colors.light.text)],
    [header,css.replace('--nstu-spacing-xs: 4px;','--nstu-spacing-xs: 40px;')],
    [header,css.replace('--nstu-controls-icon-stroke: 1.7;','--nstu-controls-icon-stroke: 1.7px;')],
    [header.replace('k_weights_medium = 500.0f','k_weights_medium = 600.0f'),css],
    [header.replace('system-ui, sans-serif','monospace'),css],
    [header,css.replace('100 110 120 130;','100 110 120;')],
    [header.replace('k_reference_dpi = 96.0f','k_reference_dpi = 120.0f'),css]
  ];
  for (const [badHeader,badCss] of faults) assert.throws(()=>assertParity(badHeader,badCss));
});

test('two complete generation runs are deterministic and preserve input and authored sentinels', async () => {
  const root = await fixture();
  try {
    const sentinel = join(root,'authored.css'); await writeFile(sentinel, 'Do not change authored components');
    const before = digest(await readFile(sentinel));
    assert.equal(run(root).status,0); const first = await rendered(root);
    assert.equal(run(root).status,0); const second = await rendered(root);
    assert.deepEqual(second,first); assert.equal(digest(await readFile(sentinel)), before);
    assert.deepEqual(await readFile(join(root,'config/ui-desktop-tokens.json')), canonical);
    assert.equal(run(root,'--check').status,0);
  } finally { await rm(root,{recursive:true,force:true}); }
});

test('isolated palette, zero/fractional metric mutations propagate to both outputs and digest', async () => {
  const root = await fixture();
  try {
    assert.equal(run(root).status,0); const before=await rendered(root);
    const modified=structuredClone(tokens); modified.colors.light['action-text']='#123456'; modified.spacing.xs=0; modified.controls.iconStroke=1.25;
    const input=Buffer.from(JSON.stringify(modified)); await writeFile(join(root,'config/ui-desktop-tokens.json'),input);
    assert.equal(run(root).status,0); const after=await rendered(root);
    assert.notDeepEqual(after,before);
    assert(after[0].includes(Buffer.from('0x123456')) && after[1].includes(Buffer.from('--nstu-color-action-text: #123456')));
    assert(after[0].includes(Buffer.from('k_xs = 0.0f')) && after[1].includes(Buffer.from('--nstu-spacing-xs: 0px')));
    assert(after[0].includes(Buffer.from('k_icon_stroke = 1.25f')) && after[1].includes(Buffer.from('--nstu-controls-icon-stroke: 1.25;')));
    assert(after.every(out=>out.includes(Buffer.from(digest(input)))));
    assertParity(after[0].toString(),after[1].toString(),modified,input);
  } finally { await rm(root,{recursive:true,force:true}); }
});

test('missing and stale check mode returns nonzero and performs no writes', async () => {
  const root=await fixture();
  try {
    assert.notEqual(run(root,'--check').status,0);
    for (const p of outputs) await assert.rejects(readFile(join(root,p)),{code:'ENOENT'});
    assert.deepEqual((await readdir(root)).sort(),['config']);
    assert.equal(run(root).status,0); await writeFile(join(root,outputs[0]),'stale');
    const before=await rendered(root);assert.notEqual(run(root,'--check').status,0);
    assert.deepEqual(await rendered(root),before);
  } finally { await rm(root,{recursive:true,force:true}); }
  for (const missing of outputs) {
    const single = await fixture();
    try {
      assert.equal(run(single).status,0);
      await rm(join(single,missing));
      const other = outputs.find(p=>p!==missing), preserved = await readFile(join(single,other));
      const before = await readdir(join(single,missing,'..'));
      assert.notEqual(run(single,'--check').status,0);
      await assert.rejects(readFile(join(single,missing)),{code:'ENOENT'});
      assert.deepEqual(await readFile(join(single,other)),preserved);
      assert.deepEqual(await readdir(join(single,missing,'..')),before);
      assert.deepEqual(await readFile(join(single,'config/ui-desktop-tokens.json')),canonical);
    } finally { await rm(single,{recursive:true,force:true}); }
  }
});

test('invalid schema, colors, metric and font reject before creating or changing outputs', async () => {
  const cases=[t=>{t.schemaVersion=2;},t=>{delete t.colors.dark.focus;},t=>{delete t.colors.dark.focus;delete t.colors.light.focus;},t=>{t.colors.light.text='#xyz';},t=>{t.geometry.titlebar=-1;},t=>{t.controls.iconStroke='small';},t=>{t.typography.family='Bad"; injected';},t=>{t.typography.weights={};}];
  for (const group of ['spacing','radius','geometry','controls','typography']) {
    for (const key of Object.keys(tokens[group])) cases.push(t=>{delete t[group][key];});
    cases.push(t=>{t[group].unexpectedMetric=1;});
  }
  for (const key of Object.keys(tokens.typography.weights)) cases.push(t=>{delete t.typography.weights[key];});
  for(const mutate of cases) for(const present of [false,true]) {
    const root=await fixture();
    try {
      if(present) assert.equal(run(root).status,0);
      const before=present?await rendered(root):null;const invalid=structuredClone(tokens);mutate(invalid);
      await writeFile(join(root,'config/ui-desktop-tokens.json'),JSON.stringify(invalid));
      assert.notEqual(run(root).status,0);
      if(present) assert.deepEqual(await rendered(root),before);
      else {
        for(const p of outputs) await assert.rejects(readFile(join(root,p)),{code:'ENOENT'});
        assert.deepEqual((await readdir(root)).sort(),['config']);
      }
    } finally { await rm(root,{recursive:true,force:true}); }
  }
});

test('output symlink and hardlink aliases cannot overwrite input or another output', async () => {
  for (const kind of ['symlink','hardlink']) for (const targetKind of ['input','output']) {
    const root = await fixture();
    try {
      await mkdir(join(root,'server/app'),{recursive:true});
      await mkdir(join(root,'exam/shared'),{recursive:true});
      const target = join(root,targetKind==='input' ? 'config/ui-desktop-tokens.json' : outputs[1]);
      if (targetKind==='output') await writeFile(target,'preserved CSS sentinel');
      if (kind==='symlink') await symlink(target,join(root,outputs[0]),'file');
      else await link(target,join(root,outputs[0]));
      const beforeTarget = await readFile(target), beforeInput = await readFile(join(root,'config/ui-desktop-tokens.json'));
      const result = run(root);
      assert.notEqual(result.status,0,kind+'/'+targetKind+' must reject before writing');
      assert.match(result.stderr,/alias|symlink|link/i);
      assert.deepEqual(await readFile(target),beforeTarget);
      assert.deepEqual(await readFile(join(root,'config/ui-desktop-tokens.json')),beforeInput);
      if (targetKind==='input') await assert.rejects(readFile(join(root,outputs[1])),{code:'ENOENT'});
    } finally { await rm(root,{recursive:true,force:true}); }
  }
});

test('generator rejects output ancestor symlink escape without writing outside root', async () => {
  const root=await fixture(), outside=await mkdtemp(join(tmpdir(),'nstu-outside-'));
  try {
    await symlink(outside,join(root,'server'),process.platform==='win32'?'junction':'dir');
    const result=run(root);assert.notEqual(result.status,0);assert.match(result.stderr,/outside.*root|escape/i);
    await assert.rejects(readFile(join(outside,'app/ui_desktop_tokens.hpp')),{code:'ENOENT'});
    await assert.rejects(readFile(join(root,outputs[1])),{code:'ENOENT'});
  } finally { await rm(root,{recursive:true,force:true}); await rm(outside,{recursive:true,force:true}); }
});
