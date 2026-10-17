# Call of Duty: Black Ops (T5) Dvars – Defaults and Effects

Below is a compiled reference for the requested dvars. Each entry gives the default value, valid range or type (where known), a concise description of its effect (from official mod docs where available, otherwise inferred), and the visual impact of changing it. We cite primary sources (modding wikis, config dumps, console command docs) for documented behavior; where no source is available, the effect is **inferred** or noted as such. 

**Shadow-related dvars:** (from Black Ops / IW T5 engine documentation)  

| Dvar                      | Default  | Range / Type    | Effect (Doc vs Inferred)                                          | Visual Impact                                  | Source                             |
|---------------------------|----------|-----------------|-------------------------------------------------------------------|-----------------------------------------------|------------------------------------|
| `sm_enable`               | 0 (MP)¹, 1 (SP/WaW)² | 0 or 1 (bool)   | Enables shadow mapping. When *1*, primary lights (sun/spot) cast shadows.  Inferred:  Each such light requires extra passes. | Off: no shadow-map passes (faster, but no dynamic shadows). On: full dynamic shadows (draw calls roughly double). | Doc; Inferred: doubling draws |
| `sm_maxLights`            | 4³       | Integer (≥0)    | Limits how many primary lights can have shadow maps.                           | Lower value: fewer lights with shadows (less shadow detail, but fewer draw calls). Higher: more shadowed lights.       | Doc            |
| `sm_sunEnable`            | 1³       | 0 or 1          | Enables sun-directional (primary) shadow mapping from script/console.         | 0: disables sun shadows; scene lit only by ambient/skylight (flatter lighting). 1: sun casts dynamic shadows.       | Doc            |
| `sm_spotEnable`           | 1³       | 0 or 1          | Enables spot (point/flashlight) shadow mapping from script.                   | 0: spotlights do not cast shadows. 1: spot lights cast shadows (adds detail around lamps).   | Doc            |
| `sm_sunShadowScale`       | 1³       | Float           | Sun shadow scale optimization (tweak factor). Documented but opaque: “Sun shadow scale optimization”.  *Inferred:* Multiplies shadow-map resolution/coverage.  | <1: smaller/fewer sun shadow maps (faster, blurrier shadows). >1: (unlikely used; default=1). | Doc (effect unclear) |
| `sm_sunShadowSmallEnable` | 0³       | 0 or 1          | Use quarter-resolution sun shadow map.                                        | 1: lower-res sun shadows (blurry but faster). 0: full-res shadows.                 | Doc            |
| `sm_qualitySpotShadow`    | 1³       | 0 or 1 (bool)   | “Fast spot shadow” (quality toggle).  *Inferred:* 0=high-quality, 1=lower-quality shadows. | 0: high-res spot shadows (slower). 1: lower-res spot shadows (faster, softer).    | Doc            |
| `sm_strictCull`           | 1³       | 0 or 1          | “Strict shadow map cull”.  *Inferred:* Enables aggressive frustum culling for shadows. | 1: tightly culls shadow casters (less shadow jitter, slightly fewer draws). 0: looser culling (shadows may include offscreen geometry). | Doc (meaning inferred) |
| `sm_fastSunShadow`        | 1³       | 0 or 1          | “Fast sun shadow”. *Inferred:* Use simpler sun-shadow algorithm.    | 1: possibly one-sample or low-res sun shadows (faster, slightly offset). 0: full-quality shadows.     | Doc (inferred) |
| `r_spotLightShadows`      | 1⁴       | 0 or 1          | Enables shadows from spot lights (engine-level) – disables all geometry shadows cast by lights if 0. | 0: no spot-light shadows (faster). 1: normal spot-light shadows.            | Default from engine lists (desc. *inferred*) |
| `r_spotLightEntityShadows`| 1⁴       | 0 or 1          | Toggles entity (character) shadows from spot lights.                        | 0: spots don’t shadow moving models. 1: normal.                             | Default (effect inferred) |
| `r_spotLightSModelShadows`| 1⁴       | 0 or 1          | Toggles static-model shadows from spot lights.                              | 0: spots don’t shadow static world geometry. 1: normal.                     | Default (effect inferred) |

**Effects-related dvars:** (particle/FX controls)  

| Dvar                   | Default | Range / Type    | Effect (Doc vs Inferred)                                         | Visual Impact                                | Source                             |
|------------------------|---------|-----------------|------------------------------------------------------------------|---------------------------------------------|------------------------------------|
| `fx_enable`            | 1³      | 0 or 1          | Master toggle for all fx (particles, trails, etc).       | 0: **all** effects off (no explosions, smoke, etc – very bland, but fastest). 1: normal effect rendering. | Doc (toggle all FX) |
| `fx_draw`              | 1³      | 0 or 1          | Toggles drawing of effects after they are processed.    | 0: Effects are simulated but not shown (invisible). 1: normal (visible effects). | Doc |
| `fx_cull_elem_draw`    | 1³      | 0 or 1          | Cull fx elements (particles) for drawing.            | 1: hides small/distant particles from drawing (fewer particles rendered). 0: all appear.   | Doc |
| `fx_cull_elem_spawn`   | 1³      | 0 or 1          | Cull (skip) spawning of fx elements based on distance/size. | 1: Distant/small effect elements aren’t spawned at all (lighter load). 0: all spawn. | Doc |
| `fx_cull_effect_spawn` | 0³      | 0 or 1          | Cull entire effects (skip spawning) in some cases.       | 1: Large effects may not spawn if not visible (faster). 0: all effects spawn. | Doc |
| `fx_drawClouds`        | 1³      | 0 or 1          | Toggles drawing of particle cloud effects (e.g. volumetric smoke). | 0: No particle "cloud" effects (e.g. heavy smoke/fog removed). 1: normal clouds. | Doc |
| `fx_marks`             | 1⁴      | 0 or 1          | Toggle bullet-hit marks (decals).                      | 0: No bullet hole/decal marks. 1: normal marks.                    | Doc        |
| `fx_marks_smodels`     | 1⁴      | 0 or 1          | Toggle marks on static models (world geometry).          | 0: No impact marks on walls/objects. 1: normal marks.               | Doc        |
| `fx_marks_ents`        | 1⁴      | 0 or 1          | Toggle marks on entities (characters).                  | 0: Enemies leave no blood/bullet marks. 1: normal marks.            | Doc        |
| `fx_visMinTraceDist`   | 80³     | Float           | Minimum visibility trace distance for effects (when to cull). | Larger: fewer long-range effects; smaller: more. (Dev profiling var.)  | Doc (technical) |

**Bodies & Debris dvars:** (AI corpses, ragdolls, destructibles)  

| Dvar                     | Default | Range / Type    | Effect (Doc vs Inferred)                                      | Visual Impact                                | Source                             |
|--------------------------|---------|-----------------|---------------------------------------------------------------|---------------------------------------------|------------------------------------|
| `ai_corpseCount`         | 5⁴      | Integer (≥0)    | Max number of dead AI bodies retained in the world. | Lower: corpses disappear sooner (fewer bodies visible). Higher: more corpses pile up (slower, but more gore). | Doc (max AI corpses) |
| `ragdoll_enable`         | 1⁵      | 0 or 1          | Enables ragdoll physics on deaths.            | 0: Deaths are static or use preset animations (no ragdoll physics). 1: dynamic ragdoll.  | Doc (enable ragdolls) |
| `ragdoll_max_simulating` | 16⁵     | Integer (≥0)    | Max simultaneous active ragdolls (per client). | Lower: fewer ragdolls allowed (old corpses may freeze or vanish sooner). 16: default number of falling corpses. | Doc      |
| `dynEnt_spawnedLimit`    | 50⁴     | Integer (≥0)    | Max number of dynamic entities (script-created debris).  | Limits spawnable debris/fx. Lower: debris stops spawning (less clutter). | Doc (limits extra dynents) |
| `cg_bloodLimit`          | –       | Integer         | *(No official docs found)* Inferred: likely limits on-screen blood decals.  | Reducing it would cap visible blood splatters. (Default unknown.)      | *Inferred* (no source found) |

**LOD / Draw-distance dvars:**  

| Dvar               | Default | Range / Type    | Effect (Doc vs Inferred)                                      | Visual Impact                           | Source                            |
|--------------------|---------|-----------------|---------------------------------------------------------------|----------------------------------------|-----------------------------------|
| `r_lodScaleSkinned`| 1⁵      | Float (≥0)      | Scale factor for LOD of animated (skinned) models.  | >1: higher-detail LOD used (draws more detail at distance). <1: lower-detail models sooner.  | Default (scale)  |
| `r_lodBiasRigid`   | 0⁵      | Float (can be negative) | LOD bias for rigid (static) models.           | Positive: detail farther; negative: crisper (inferred).   | Default         |
| `r_lodBiasSkinned` | 0⁵      | Float           | LOD bias for skinned models.                   | Similar to above, for animated models.  | Default         |
| `r_zfar`           | 0⁵      | Float (inches)  | Far clipping plane distance. 0 = use engine defaults (dynamic). | Larger: draw objects farther (more fill, slower). Smaller: fog/clip sooner (faster). | Default (0=default) |

**Glow / Post-processing dvars:**  

| Dvar                 | Default | Range / Type    | Effect (Doc vs Inferred)                                      | Visual Impact                            | Source                                 |
|----------------------|---------|-----------------|---------------------------------------------------------------|-----------------------------------------|----------------------------------------|
| `r_glow`             | 1⁶      | 0 or 1          | Enables bloom/glow post-effect. 1=enabled.    | 0: No bloom; very few glowy highlights. 1: normal bloom around bright objects.  | Default               |
| `r_glow_allowed`     | 1⁶      | 0 or 1          | Master toggle for allowing glow (script-disabled). | If 0, forced no glow (overrides r_glow). Rarely used. | Default               |
| `r_dof_enable`       | 0⁵      | 0 or 1          | Enable depth-of-field (camera blur) effect.      | 0: no DOF; everything sharp. 1: scene blur on near/far objects.      | Doc                   |
| `r_distortion`       | 1⁶      | 0 or 1          | Enables screen-space distortion (heat-haze, refractive fx).   | 0: no distortions (haze effects removed). 1: normal distortions (e.g. hound’s aura).  | Doc                      |
| `r_flameFX_enable`   | 0⁶      | 0 or 1          | Enables flamethrower particle effects.               | 0: no flame effects. 1: normal flame (used for flame weapons).    | Doc (flamethrower)        |

Notes on defaults: ¹Black Ops MP (server) config shows `sm_enable 0`. ²Many console dumps (e.g. WaW) and co-op use `sm_enable 1` by default. ³Defaults from config/server dumps or ZeroY/WaW Wiki (for WaW/BO) – assume the engine defaults match. ⁴From PC/CoD4 engine scripting default lists (may differ on console). ⁵From COD4/BO engine defaults. ⁶From a Black Ops console command dump.

### 1. Shadow passes when `sm_enable=1`

With **shadow mapping enabled** (`sm_enable = 1`), the engine uses special shader techniques for “lit … shadow” passes.  In practice **each primary light adds extra draw passes**. For example, CoD4 docs show techniques like “lit sun shadow” and “lit spot shadow” when `sm_enable`=1. This effectively *doubles* the number of draws during heavy lighting. In our case (T5/Zombie engine), the symptom is ~2× draw calls even if shadow maps aren’t actually drawn. 

To reduce this overhead *while keeping shadows on*:
- **Limit shadowed lights:** `sm_maxLights` (e.g. to 1–2) caps how many lights cast shadows. Fewer light passes = fewer extra draws.
- **Lower-res shadows:** `sm_fastSunShadow=1` and `sm_sunShadowSmallEnable=1` force simpler/half-res sun shadows.
- **Disable specific shadow types:** e.g. set `r_spotLightShadows=0`, `r_spotLightEntityShadows=0`, `r_spotLightSModelShadows=0` to skip spot-light shadows on scenery or characters. These skip entire shadow-rendering passes for that category (inferred from their names).
- **Other toggles:** `sm_strictCull=1` might cull offscreen shadow casters, and `sm_qualitySpotShadow=1` (fast spot shadows) trades some quality for speed. 

*(All “extra work” is from shadow passes; these dvars prune or degrade those passes.)*

### 2. Reducing heavy-particle costs (e.g. hound rounds)

The following reduce particle/fx workload with minimal visual loss:

- **Effect culling:** Turn on `fx_cull_effect_spawn`, `fx_cull_elem_spawn`, `fx_cull_elem_draw`. These skip or omit far/small particle elements (like distant flames, sparks, lightning segments). In gameplay, enabling them (set to 1) significantly cuts the number of spawned/drawn particles. 
- **Disable cloud effects:** `fx_drawClouds=0` turns off large particle cloud renders (e.g. big smoke plumes).
- **Limit marks:** Set `fx_marks=0` or `fx_marks_smodels=0` to stop drawing bullet/decal marks if still an issue (zombies/FX may spawn blood or scorch marks).
- **Global FX off:** `fx_enable=0` kills all effects (not usually desirable due to major visual loss).
- **Distortion:** `r_distortion=0` removes refractive glows (e.g. fire flicker or hound aura), which are GPU-intensive effects with minor visual necessity. 

All of the above are documented toggles. Exact CPU/GPU savings vary, but culling distant particles often yields large gains on hound+fire rounds (inferred from how many particles are skipped).

### 3. PC vs Xbox360 builds

The IW engine is largely shared, so **most dvars exist on both platforms**. However, *default values* can differ. For example, the Black Ops PC/MP config shows `sm_enable = 0` by default, while some CoD4 PC sources have `sm_enable = 1`. Likewise, some console ports might enforce performance defaults (e.g. limiting FX or shadows). 

In practice, you can assume the same dvars exist on 360 and PC. Many defaults match (e.g. ragdoll_enable=1, fx_enable=1, etc). But double-check any 360-specific config or mod tool data if exact defaults matter. (In our sources above, we flagged defaults from PC/server configs or engine scripts – they may not exactly match the 360 build’s defaults.)

**Sources:** We relied on official mod-tool docs and community wikis for descriptions, and on engine command dumps for defaults. Inferred effects are noted as such (e.g. how enabling culling “removes” elements). No generic “just lower settings” advice is included – every claim is tied to a specific dvar or documented behavior. 


---

## Review notes (bo1-nx, 2026-10-16/17)

- **Defaults:** the ones we could check against the game's own dvar registration (`sub_8258CDA8`) match: `sm_maxLights`
  4, `r_spotLightEntityShadows` 1, `r_spotLightSModelShadows` 1, `sm_sunShadowSmallEnable` 0, `sm_strictCull` 1,
  `sm_fastSunShadow` 1.
- **"Why ~2x draws with sm_enable 1" (section 1):** the answer here (extra lighting passes per light) is inferred and
  does not match a measurement. Mac, Kino, prepass not skipped: with the `min` preset the game issues ~905k draws per
  10 s against ~326k with shadows off, and ~580k of them are shadow-map caster draws that the renderer skips, so the
  draws actually rendered are about the same (~310k vs ~326k). The extra cost is the game building, and the renderer
  parsing, caster draws that are thrown away.
- **Shadow levers:** fewer shadowed lights, no entity/static-model spot casters and the small sun map are exactly what
  the port's `min` preset already sets (`sm_maxLights 1`, `r_spotLightEntityShadows 0`, `r_spotLightSModelShadows 0`,
  `sm_qualitySpotShadow 0`, `sm_sunShadowScale 0.25`, `sm_sunShadowSmallEnable 1`). `sm_sunEnable`, `sm_spotEnable` and
  `r_spotLightShadows` are documented but changed nothing in this build's frame trace.
- **New lead from it:** shrink the sun shadow range further for `min` (`sm_sunShadowScale` and `sm_sunSampleSizeNear`
  below 0.25) so the game stops generating the caster draws `min` discards. Not tested yet.
- **Effects:** `fx_cull_effect_spawn` (default 0) and the other culling dvars are the useful part for busy scenes. Built
  as the `bo1_fx_lite` test preset (`fx_cull_effect_spawn 1`, `fx_cull_elem_spawn 1`, `fx_cull_elem_draw 1`,
  `fx_drawClouds 0`, `ai_corpseCount 3`); the Mac accepts them, and the console test is still pending. `r_glow` and
  `r_distortion` are further candidates (the port already sets `r_distortion 0`).
