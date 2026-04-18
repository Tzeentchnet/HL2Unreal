# Master material assets live here (Phase 11a)

The plugin expects six (seven with decal) `UMaterial` assets under this folder, mounted at
`/HL2BSPImporter/MasterMaterials/`:

- `M_HL2_Lit.uasset`
- `M_HL2_LitMasked.uasset`
- `M_HL2_LitTranslucent.uasset`
- `M_HL2_LitDecal.uasset` (Deferred Decal domain)
- `M_HL2_WorldVertexBlend.uasset`
- `M_HL2_VertexLit.uasset`
- `M_HL2_Unlit.uasset`

Author them in the Unreal editor against the parameter contract in
`../../Resources/MasterMaterials/README.md`. See `MaterialBuilderInstructions.md`
in this folder for the step-by-step editor walkthrough. Once present this
placeholder file can be deleted; UE will mount the folder automatically because
the plugin's `.uplugin` declares `"CanContainContent": true`.

If the assets are missing, the importer falls back to the per-slot settings
overrides (Project Settings → HL2 BSP Importer → Materials | Synthesis |
Parents) and ultimately logs `No parent material configured for shader '<x>'`
and skips synthesis with no crash.
