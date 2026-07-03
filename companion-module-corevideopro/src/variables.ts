import type { CompanionVariableDefinition } from '@companion-module/base'

/** ControlState string/number fields exposed as Companion variables (e.g. $(corevideopro:activeSceneId)). */
export const VARIABLE_FIELDS: { id: string; name: string }[] = [
	{ id: 'zoomStatus', name: 'Zoom status' },
	{ id: 'engineStatus', name: 'Engine status' },
	{ id: 'commandStatus', name: 'Command status' },
	{ id: 'activeSceneId', name: 'Program (active) scene id' },
	{ id: 'previewSceneId', name: 'Preview scene id' },
	{ id: 'viewMode', name: 'View mode' },
	{ id: 'multiviewLayoutMode', name: 'Multiview layout mode' },
	{ id: 'multiviewTileCount', name: 'Multiview tile count' },
	{ id: 'audioMonitorVolume', name: 'Audio monitor volume' },
	{ id: 'recording', name: 'Recording (bool)' },
	{ id: 'streaming', name: 'Streaming (bool)' },
	{ id: 'engineOn', name: 'Engine on (bool)' },
]

export function buildVariables(): CompanionVariableDefinition[] {
	return VARIABLE_FIELDS.map((f) => ({ variableId: f.id, name: f.name }))
}
