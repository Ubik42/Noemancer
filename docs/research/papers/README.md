# Research paper archive

> Document class: Historical source archive. Papers and project-positioning notes are research evidence, not the active architecture or roadmap.

## GameEngineBench

- Title: *GameEngineBench: Evaluating Coding Agents on Real C++ Runtime Environments*
- Authors: Brian La, Sejoon Chang, Ben Kim, Junyoung Bae, Aamish Ahmad Beg, Sei Chang, Gonzalo Gonzalez-Pumariega, Kanav Goyal
- Version: arXiv v2, 2026-07-15
- arXiv: [2607.03525](https://arxiv.org/abs/2607.03525)
- Code: [Nitrode-Research/GameEngineBench](https://github.com/Nitrode-Research/GameEngineBench)
- License: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)
- Upstream PDF: [arXiv:2607.03525](https://arxiv.org/pdf/2607.03525)
- SHA-256: `320B4D2722FEF3647AE0896D9BCCE43DC42E6531CCA3391E3B11E62C0EEA7EDB`

### Why this paper anchors the project

The benchmark contains 110 native C++ implementation tasks from nine existing Unreal Engine projects. A solution must compile and pass runtime behavioral checks; the strongest evaluated configuration reaches 55.5% pass@1, and 31 tasks are unsolved by every evaluated configuration.

The most relevant observation for this engine is that failures are not limited to syntax or compilation. They cluster around multiplayer authority, state synchronization, object lifecycle, subsystem initialization, and incomplete integration with surrounding engine systems. A patch can compile and still be wrong in the running engine.

This engine therefore targets the complete Agent development loop:

```text
understand -> edit -> affected build -> hot patch -> runtime observation
           -> semantic/native debug -> behavioral verification -> evidence
```

The project should eventually maintain its own benchmark derived from this framing. It should measure more than pass/fail:

- pass@1 on scoped C++ engine tasks;
- edit-to-feedback and compile wait time;
- number of tool calls and context bytes;
- percentage of failures with a correct structural diagnosis;
- successful hot patch and rollback rate;
- completeness of the final evidence chain.

The upstream paper is linked for reproducible research reference. No endorsement by the paper authors is implied.

## AI-assisted VFX and Hybrid Pixel rendering

### Elemental Alchemist

- Title: *Elemental Alchemist: A Generative Interface for Semantic Control of Particle Systems Across Dynamic Levels of Abstraction*
- Version: arXiv v1, 2026-05-11
- arXiv: [2605.10014](https://arxiv.org/abs/2605.10014)
- Upstream PDF: [arXiv:2605.10014](https://arxiv.org/pdf/2605.10014)
- SHA-256: `4F861262A86FC4970AC3978B5EC29FE6DF4683B45FC81C7C8ADFB221C44489C1`
- Relevance: semantic high/mid/low-level controls, bounded parameter metadata, and deterministic editable particle systems.

### ParticleGen

- Title: *ParticleGen: A Multi-Agent System for Particle Effects Generation*
- Version: arXiv v1, 2026-08-01
- arXiv: [2608.00629](https://arxiv.org/abs/2608.00629)
- Upstream PDF: [arXiv:2608.00629](https://arxiv.org/pdf/2608.00629)
- SHA-256: `24FACB1A8CCCE953268014AE52671FBD0DF78C229B2A12917B3864A1C34E66D3`
- Relevance: separates planning and parameterization, produces editable Niagara structures, and iterates using rendered diagnostic feedback.

### Pixel Art Normal Map Generation

- Title: *Analysis and Compilation of Normal Map Generation Techniques for Pixel Art*
- Version: arXiv v1, 2022-12-19
- arXiv: [2212.09692](https://arxiv.org/abs/2212.09692)
- Upstream PDF: [arXiv:2212.09692](https://arxiv.org/pdf/2212.09692)
- SHA-256: `55CE345E5348A4B920DFF67B8CC598B8B47A91ADFE15E8B5DF9BB15CCC52FF94`
- Relevance: pixel-art-specific normal estimation for lighting 2D sprites without discarding their visual style.

The three upstream papers are linked for project research and do not imply endorsement by the authors.

## Modular 2D character animation

### Sprite Sheet Diffusion

- Title: *Sprite Sheet Diffusion: Generate Game Character for Animation*
- Version: arXiv v1, 2024-12-04
- arXiv: [2412.03685](https://arxiv.org/abs/2412.03685)
- Code: [chenganhsieh/Sprite-Sheet-Diffusion](https://github.com/chenganhsieh/Sprite-Sheet-Diffusion)
- Upstream PDF: [arXiv:2412.03685](https://arxiv.org/pdf/2412.03685)
- SHA-256: `EF2910BA1DF6BA68A05B3D49168BD7E9A4CC628BC0A0234E40D33597B4B71158`
- Relevance: reference-image and pose-sequence conditioned sprite animation; useful as a generation experiment, not a deterministic asset compiler.

### SPRITETOMESH

- Title: *SPRITETOMESH: Automatic Mesh Generation for 2D Skeletal Animation Using Learned Segmentation and Contour-Aware Vertex Placement*
- Version: arXiv v1, 2026-02-24
- arXiv: [2602.21153](https://arxiv.org/abs/2602.21153)
- Upstream PDF: [arXiv:2602.21153](https://arxiv.org/pdf/2602.21153)
- SHA-256: `2228C6E66FF5E68F8B3DB932EF36DA86E56D0D2F462E41D7828496E9C603B026`
- Relevance: hybrid learned segmentation and deterministic mesh construction for converting Sprite art into skeletal-animation meshes.

### Sketch2Motion

- Title: *Sketch2Motion: Text-driven 2D Sketch to 3D Animation via Diffusion-guided Skeleton Optimization*
- Version: arXiv v1, 2026-05-27
- arXiv: [2605.28394](https://arxiv.org/abs/2605.28394)
- Upstream PDF: [arXiv:2605.28394](https://arxiv.org/pdf/2605.28394)
- SHA-256: `A8F9C48E3C2AF9AEFF3869E5BB5006A832118DF2723493B4A1473326A902C2B4`
- Relevance: combines generative motion priors with skeleton, skinning, contact, smoothness and secondary-motion constraints.

The three upstream papers are linked for project research and do not imply endorsement by the authors.
