import { combineRgb, type CompanionFeedbackDefinitions } from '@companion-module/base'

/** Minimal surface a feedback needs from the instance (avoids a circular import). */
export interface FlagSource {
	flag(id: string): boolean
}

/** The boolean ControlState fields worth lighting a button. */
export const FEEDBACK_FLAGS: { id: string; name: string; on: number; off?: number }[] = [
	{ id: 'recording', name: 'Recording active', on: combineRgb(200, 40, 40) },
	{ id: 'streaming', name: 'Streaming active', on: combineRgb(200, 40, 40) },
	{ id: 'engineOn', name: 'Capture engine on', on: combineRgb(40, 140, 60) },
	{ id: 'canTake', name: 'Take available', on: combineRgb(180, 140, 20) },
	{ id: 'lowerThirdOnAir', name: 'Lower-third on air', on: combineRgb(40, 120, 200) },
	{ id: 'automationOn', name: 'Automation on', on: combineRgb(120, 60, 200) },
	{ id: 'autoTake', name: 'Auto-take enabled', on: combineRgb(120, 60, 200) },
	{ id: 'autoAssignInputs', name: 'Auto-assign inputs enabled', on: combineRgb(120, 60, 200) },
	{ id: 'autoLowerThirds', name: 'Auto lower-thirds enabled', on: combineRgb(120, 60, 200) },
	{ id: 'autoCaptions', name: 'Auto captions enabled', on: combineRgb(120, 60, 200) },
	{ id: 'audioMonitorOn', name: 'Audio monitor on', on: combineRgb(40, 140, 60) },
	{ id: 'masterLimiterOn', name: 'Master limiter on', on: combineRgb(40, 140, 60) },
]

export function buildFeedbacks(source: FlagSource): CompanionFeedbackDefinitions {
	const defs: CompanionFeedbackDefinitions = {}
	for (const flag of FEEDBACK_FLAGS) {
		defs[flag.id] = {
			type: 'boolean',
			name: flag.name,
			defaultStyle: {
				bgcolor: flag.on,
				color: combineRgb(255, 255, 255),
			},
			options: [],
			callback: () => source.flag(flag.id),
		}
	}
	return defs
}
