import { describe, expect, it } from 'vitest'
import { normalizeManifest, fetchManifest } from './manifest.js'

const pascal = {
	Version: 1,
	Root: '/cvp',
	Actions: [
		{
			Id: 'scene.select',
			Title: 'Select scene',
			Description: 'd',
			OscAddress: '/cvp/scene/select',
			Params: [{ Name: 'sceneId', Type: 'string', Required: true, Description: 'id' }],
		},
		{ Id: 'transport.take', Title: 'Take', Description: 't', OscAddress: '/cvp/transport/take', Params: [] },
	],
	FeedbackFields: ['recording', 'ohg/slot/{slot}/tally'],
}
const camel = {
	version: 1,
	root: '/cvp',
	actions: [
		{
			id: 'scene.select',
			title: 'Select scene',
			description: 'd',
			oscAddress: '/cvp/scene/select',
			params: [{ name: 'sceneId', type: 'string', required: true, description: 'id' }],
		},
		{ id: 'transport.take', title: 'Take', description: 't', oscAddress: '/cvp/transport/take', params: [] },
	],
	feedbackFields: ['recording', 'ohg/slot/{slot}/tally'],
}

describe('normalizeManifest', () => {
	it('reads the PascalCase manifest the Windows and Mac servers serve', () => {
		const m = normalizeManifest(pascal)
		expect(m.actions.map((a) => a.id)).toEqual(['scene.select', 'transport.take'])
		expect(m.actions[0].params[0]).toMatchObject({ name: 'sceneId', type: 'string', required: true })
		expect(m.actions[0].title).toBe('Select scene')
		expect(m.feedbackFields).toEqual(['recording', 'ohg/slot/{slot}/tally'])
	})
	it('reads a camelCase manifest', () => {
		expect(normalizeManifest(camel)).toEqual(normalizeManifest(pascal))
	})
	it('tolerates missing or malformed parts', () => {
		expect(normalizeManifest({}).actions).toEqual([])
		expect(normalizeManifest(null).actions).toEqual([])
		expect(normalizeManifest({ Actions: [{ Title: 'no id' }, { Id: 'x' }] }).actions).toEqual([
			{ id: 'x', title: 'x', description: '', oscAddress: '', params: [] },
		])
		expect(normalizeManifest({ Actions: [], FeedbackFields: null }).feedbackFields).toEqual([])
	})
})

describe('fetchManifest', () => {
	it('normalizes whatever the server returns', async () => {
		const fake = (async () => new Response(JSON.stringify(pascal), { status: 200 })) as typeof fetch
		const m = await fetchManifest('http://x', {}, fake)
		expect(m.actions).toHaveLength(2)
	})
	it('throws on a non-2xx status', async () => {
		const fake = (async () => new Response('no', { status: 401 })) as typeof fetch
		await expect(fetchManifest('http://x', {}, fake)).rejects.toThrow('HTTP 401')
	})
})
