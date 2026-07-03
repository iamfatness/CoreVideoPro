import { combineRgb, type CompanionPresetDefinitions } from '@companion-module/base'

const white = combineRgb(255, 255, 255)
const black = combineRgb(0, 0, 0)

/** A handful of ready-to-drop buttons wiring the common actions to their feedback. */
export function buildPresets(): CompanionPresetDefinitions {
	const presets: CompanionPresetDefinitions = {}

	presets['take'] = {
		type: 'button',
		category: 'Transport',
		name: 'Take',
		style: { text: 'TAKE', size: '18', color: white, bgcolor: combineRgb(40, 40, 40) },
		steps: [{ down: [{ actionId: 'transport.take', options: {} }], up: [] }],
		feedbacks: [{ feedbackId: 'canTake', options: {}, style: { bgcolor: combineRgb(180, 140, 20), color: black } }],
	}

	presets['record'] = {
		type: 'button',
		category: 'Transport',
		name: 'Record toggle',
		style: { text: 'REC', size: '18', color: white, bgcolor: combineRgb(40, 40, 40) },
		steps: [{ down: [{ actionId: 'transport.record.toggle', options: {} }], up: [] }],
		feedbacks: [{ feedbackId: 'recording', options: {} }],
	}

	presets['stream'] = {
		type: 'button',
		category: 'Transport',
		name: 'Stream toggle',
		style: { text: 'STREAM', size: '14', color: white, bgcolor: combineRgb(40, 40, 40) },
		steps: [{ down: [{ actionId: 'transport.stream.toggle', options: {} }], up: [] }],
		feedbacks: [{ feedbackId: 'streaming', options: {} }],
	}

	presets['engine'] = {
		type: 'button',
		category: 'Transport',
		name: 'Engine toggle',
		style: { text: 'ENGINE', size: '14', color: white, bgcolor: combineRgb(40, 40, 40) },
		steps: [{ down: [{ actionId: 'transport.engine.toggle', options: {} }], up: [] }],
		feedbacks: [{ feedbackId: 'engineOn', options: {} }],
	}

	presets['autoassign'] = {
		type: 'button',
		category: 'Automation',
		name: 'Auto-assign inputs ON',
		style: { text: 'AUTO\\nASSIGN', size: '14', color: white, bgcolor: combineRgb(40, 40, 40) },
		steps: [{ down: [{ actionId: 'automation.autoAssignInputs.set', options: { on: true } }], up: [] }],
		feedbacks: [{ feedbackId: 'autoAssignInputs', options: {} }],
	}

	presets['lowerthird'] = {
		type: 'button',
		category: 'Graphics',
		name: 'Lower-third toggle',
		style: { text: 'LOWER\\n3RD', size: '14', color: white, bgcolor: combineRgb(40, 40, 40) },
		steps: [{ down: [{ actionId: 'graphics.lowerThird.toggle', options: {} }], up: [] }],
		feedbacks: [{ feedbackId: 'lowerThirdOnAir', options: {} }],
	}

	return presets
}
