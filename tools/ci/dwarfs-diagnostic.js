'use strict';

const fs = require('fs');

module.exports = async function run({ github, context, core }) {
  const marker = '<!-- ipasim-dwarfs-reader-diagnostic -->';
  const stage = process.env.DIAGNOSTIC_STAGE || 'DwarFS acceptance';
  const outcome = process.env.DIAGNOSTIC_OUTCOME || 'failure';
  const reportSuccess = process.env.DIAGNOSTIC_REPORT_SUCCESS === 'true';
  const logPath = process.env.DIAGNOSTIC_LOG || '';
  const successBody = process.env.DIAGNOSTIC_SUCCESS_BODY || '';

  if (outcome === 'success' && !reportSuccess) {
    core.info(`${stage} passed; leaving the persistent PR diagnostic unchanged.`);
    return;
  }

  let text = 'diagnostic log was not produced';
  if (logPath && fs.existsSync(logPath)) {
    text = fs.readFileSync(logPath, 'utf8');
  }
  const lines = text.split(/\r?\n/);
  const useful = lines.filter(line =>
    /error|fatal|failed|exception|cannot|missing|mismatch|not found|required|read-tree|vcpkg|cmake|ninja|dwarfs|runtime|loader|dependency|symbol|dylib|framework|ipasim-probe|mach-o|integrity|brew/i.test(line)
  ).slice(-180);
  const excerpt = (useful.length ? useful : lines.slice(-180)).join('\n').slice(-20000);

  const body = outcome === 'success'
    ? `${marker}\n## RuntimeRoot DwarFS diagnostic\n\n✅ **${stage} passed** on \`${context.sha}\`.\n\n${successBody}`
    : `${marker}\n## RuntimeRoot DwarFS diagnostic\n\n❌ **${stage} failed** on \`${context.sha}\`.\n\n### Next actionable output\n\`\`\`text\n${excerpt}\n\`\`\`\n\nNo RuntimeRoot path is excluded, rewritten, mounted, or extracted to make this test pass. The workflow fails normally after publishing this diagnostic.`;

  const { owner, repo } = context.repo;
  const issue_number = context.issue.number;
  const comments = await github.paginate(
    github.rest.issues.listComments,
    { owner, repo, issue_number, per_page: 100 }
  );
  const existing = comments.find(comment => comment.body && comment.body.includes(marker));
  if (existing) {
    await github.rest.issues.updateComment({ owner, repo, comment_id: existing.id, body });
  } else {
    await github.rest.issues.createComment({ owner, repo, issue_number, body });
  }
};
