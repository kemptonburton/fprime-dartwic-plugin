# Bundled plugin assets

Files here ship inside each installed plugin's `assets/` folder. They belong to
the plugin and may be replaced or removed by an update or uninstall.

Save shared custom files through the SDK plugin workspace asset helpers in
`workspace/assets/plugins/<plugin-id>/`. Keep project-specific settings in the
project settings scope, and secrets, recordings and caches in instance storage.
Generic workspace, project and installation APIs remain available for deliberate
advanced workflows; automatic installation never injects files into a workspace.
