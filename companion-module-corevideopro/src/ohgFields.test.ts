import { describe, expect, it } from 'vitest'
import { expandFeedbackFields, isTallyField, variableIdFor } from './ohgFields.js'

describe('expandFeedbackFields', () => {
	it('expands a {slot} template into 10 fields by default', () => {
		const fields = expandFeedbackFields(['ohg/slot/{slot}/tally'])
		expect(fields).toHaveLength(10)
		expect(fields[0]).toBe('ohg/slot/1/tally')
		expect(fields[9]).toBe('ohg/slot/10/tally')
	})

	it('respects a custom slot count', () => {
		const fields = expandFeedbackFields(['ohg/slot/{slot}/name'], 3)
		expect(fields).toEqual(['ohg/slot/1/name', 'ohg/slot/2/name', 'ohg/slot/3/name'])
	})

	it('passes through ohg/ fields with no {slot} placeholder unchanged', () => {
		const fields = expandFeedbackFields(['ohg/program/mode', 'ohg/health/engine'])
		expect(fields).toEqual(['ohg/program/mode', 'ohg/health/engine'])
	})

	it('ignores non-ohg/ fields entirely', () => {
		const fields = expandFeedbackFields([
			'recording',
			'input/{slot}/inShow',
			'ohg/queue/current',
		])
		expect(fields).toEqual(['ohg/queue/current'])
	})

	it('expands multiple templates and concatenates them in order', () => {
		const fields = expandFeedbackFields(['ohg/slot/{slot}/tally', 'ohg/gallery/smart'], 2)
		expect(fields).toEqual(['ohg/slot/1/tally', 'ohg/slot/2/tally', 'ohg/gallery/smart'])
	})
})

describe('variableIdFor', () => {
	it('converts ohg/slot/3/tally to ohg_slot_3_tally', () => {
		expect(variableIdFor('ohg/slot/3/tally')).toBe('ohg_slot_3_tally')
	})

	it('converts a nested capability field', () => {
		expect(variableIdFor('ohg/capabilities/registry/state')).toBe('ohg_capabilities_registry_state')
	})

	it('converts a field with no slashes unchanged', () => {
		expect(variableIdFor('recording')).toBe('recording')
	})
})

describe('isTallyField', () => {
	it('is true for a field ending in /tally', () => {
		expect(isTallyField('ohg/slot/3/tally')).toBe(true)
	})

	it('is false for a non-tally field', () => {
		expect(isTallyField('ohg/slot/3/name')).toBe(false)
		expect(isTallyField('ohg/program/mode')).toBe(false)
	})
})
