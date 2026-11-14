export const meta = {
  name: 'cog-dev',
  description: 'Autonomous cog-dev orchestrator loop',
  phases: [
    { title: 'Discover', detail: 'Orchestrator scans queue and decides next action' },
    { title: 'Execute', detail: 'Dispatch agent or run operation' },
  ],
};

const SKILL_DIR = '/home/user/.agents/skills/cog-dev';
const SCRIPTS_DIR = SKILL_DIR + '/scripts';

const DECISION_SCHEMA = {
  type: 'object',
  properties: {
    action: { type: 'string', enum: ['dispatch', 'merge', 'park', 'ask', 'done'] },
    agent: { type: 'string', enum: ['architect', 'writer', 'critic', 'git-merge', 'git-ops', 'orchestrator', 'none'] },
    ticket: { type: 'string' },
    worktree: { type: 'string' },
    reasoning: { type: 'string' },
    question: { type: 'string' },
  },
  required: ['action', 'reasoning'],
};

const AGENT_RESULT_SCHEMA = {
  type: 'object',
  properties: {
    status: { type: 'string', enum: ['complete', 'blocked', 'failed'] },
    summary: { type: 'string' },
    worktree: { type: 'string' },
    next_action: { type: 'string', enum: ['review', 'merge', 'escalate', 'none'] },
  },
  required: ['status', 'summary'],
};

function orchestratorPrompt(mode, focusTicket) {
  const focusLine = focusTicket
    ? 'Focus only on ticket ' + focusTicket + '. Ignore other tickets.'
    : 'Consider all open tickets.';

  return [
    'You are the Orchestrator for the cog-dev autonomous development workflow.',
    'Read ' + SKILL_DIR + '/agents/orchestrator.md for your full role instructions.',
    '',
    'Mode: ' + mode,
    '',
    focusLine,
    '',
    'Your job is to scan the current state and decide the single next action.',
    '',
    'Steps:',
    '1. Run `tix ls --json` to see open tickets.',
    '2. Run `' + SCRIPTS_DIR + '/worktree-list` to see active worktrees.',
    '3. Pick the highest-priority actionable next step.',
    '',
    'Return a JSON object with:',
    '- action: "dispatch" | "merge" | "park" | "ask" | "done"',
    '- agent: the role to dispatch (architect, writer, critic, git-merge, git-ops), or "none"',
    '- ticket: the ticket id, or "none"',
    '- worktree: the worktree name if relevant, or "none"',
    '- reasoning: brief explanation',
    '- question: if action is "ask", the question for the human',
    '',
    'Rules:',
    '- "dispatch" when an open ticket needs work and matches an agent role by its type: label.',
    '- "merge" when a ticket is in review status and has a worktree ready to merge.',
    '- "park" only if explicitly requested or if no safe forward progress is possible.',
    '- "ask" in semi mode when you need human confirmation before an irreversible action.',
    '- "done" when there are no open actionable tickets.',
    '',
    'Be conservative. Prefer small, reversible steps.',
  ].join('\n');
}

function agentPrompt(role, ticketId, worktree) {
  const worktreeHint = worktree && worktree !== 'none'
    ? 'Use the existing worktree ' + worktree + ' if it is valid.'
    : 'Create a worktree with `' + SCRIPTS_DIR + '/worktree-create ' + ticketId + ' ' + role + '`.';

  return [
    'You are the ' + role + ' for the cog-dev autonomous development workflow.',
    'Read ' + SKILL_DIR + '/agents/' + role + '.md for your full role instructions.',
    '',
    'Your assigned ticket is ' + ticketId + '.',
    worktreeHint,
    '',
    'Do the work described in your role instructions. Use the helper scripts in ' + SCRIPTS_DIR + ' for ticket and worktree operations.',
    '',
    'When finished, return a JSON object:',
    '- status: "complete" | "blocked" | "failed"',
    '- summary: what you did',
    '- worktree: the worktree name',
    '- next_action: "review" | "merge" | "escalate" | "none"',
    '',
    'If you are blocked, explain why clearly.',
  ].join('\n');
}

function parkPrompt() {
  return [
    'You are the Orchestrator for the cog-dev workflow.',
    'Park all active cog-dev worktrees by running `' + SCRIPTS_DIR + '/cog-park -m "<concise summary>"`.',
    'Choose a summary that captures the current state of all active worktrees.',
    'Report which worktrees were parked and any tickets that could not be parked.',
  ].join('\n');
}

function modelForRole(role) {
  if (role === 'architect') {
    return 'openrouter/moonshotai/kimi-k2.7-code';
  }
  return 'openrouter/~deepseek/deepseek-v4-flash-latest';
}

const mode = (args && args.mode) || 'semi';
const maxIterations = (args && args.maxIterations) || 20;
const dryRun = !!(args && args.dryRun);
const focusTicket = (args && args.ticket) || null;

if (dryRun) {
  log('Running in dry-run mode: decisions will be logged but not executed.');
}
if (focusTicket) {
  log('Focusing on ticket: ' + focusTicket);
}

let finalResult = { done: false, reason: 'max-iterations' };

for (let i = 0; i < maxIterations; i++) {
  phase('Discover');
  log('Iteration ' + (i + 1) + '/' + maxIterations);

  const decision = await agent(orchestratorPrompt(mode, focusTicket), {
    label: 'orchestrator-' + (i + 1),
    phase: 'Discover',
    schema: DECISION_SCHEMA,
    model: 'openrouter/~deepseek/deepseek-v4-flash-latest',
  });

  if (!decision) {
    log('Orchestrator returned no decision; stopping.');
    break;
  }

  log('Decision: ' + decision.action + ' ' + (decision.agent || '') + ' ' + (decision.ticket || '') + ' — ' + decision.reasoning);

  if (decision.action === 'done') {
    log('Orchestrator reports no actionable work remaining.');
    finalResult = { done: true };
    break;
  }

  if (decision.action === 'ask') {
    log('Orchestrator needs human input: ' + (decision.question || decision.reasoning));
    finalResult = { paused: true, reason: decision.reasoning, question: decision.question };
    break;
  }

  if (dryRun) {
    log('Dry-run: would execute ' + decision.action + ' for ' + (decision.ticket || 'n/a'));
    continue;
  }

  phase('Execute');

  if (decision.action === 'park') {
    const result = await agent(parkPrompt(), {
      label: 'park-' + (i + 1),
      phase: 'Execute',
      model: 'openrouter/~deepseek/deepseek-v4-flash-latest',
    });
    log(result ? 'Parked: ' + result : 'Park agent returned no output');
    continue;
  }

  if (decision.action === 'merge') {
    const role = 'git-merge';
    const ticketId = decision.ticket || 'none';
    const worktree = decision.worktree || 'none';
    const result = await agent(agentPrompt(role, ticketId, worktree), {
      label: role + '-' + ticketId,
      phase: 'Execute',
      schema: AGENT_RESULT_SCHEMA,
      model: modelForRole(role),
    });
    log(result ? 'Merge result: ' + result.status + ' — ' + result.summary : 'Merge agent failed');
    continue;
  }

  if (decision.action === 'dispatch') {
    const role = decision.agent;
    const ticketId = decision.ticket;
    if (!role || role === 'none' || !ticketId || ticketId === 'none') {
      log('Invalid dispatch decision; skipping.');
      continue;
    }
    const result = await agent(agentPrompt(role, ticketId, decision.worktree || 'none'), {
      label: role + '-' + ticketId,
      phase: 'Execute',
      schema: AGENT_RESULT_SCHEMA,
      model: modelForRole(role),
    });
    log(result ? 'Agent result: ' + result.status + ' — ' + result.summary : 'Agent failed');
    continue;
  }

  log('Unknown action: ' + decision.action + '; stopping.');
  break;
}

return finalResult;
