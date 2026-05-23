
Conversation with Gemini
what is the best way to have full transparency on our git opensource project so sponsors can se howe much money is received and where it is being spent

Building full financial transparency from day one is one of the most effective ways to build trust and encourage community backing. When contributors know exactly how their money is fueling development, they are far more likely to sustain their support.

To give your community a clear view of where funds from platforms like GitHub Sponsors and Patreon are going, here are the three most effective approaches for open-source projects.

1. The Industry Standard: Open Collective
Open Collective is the gold standard for open-source financial transparency. It is a platform specifically designed to manage and display project finances publicly.

Public Ledger: It automatically generates a public page showing your current budget, total raised, and an itemized list of every expense and income source.

Expense Submission: Contributors or maintainers can submit expenses (like server costs, domain renewals for PoseStudio.org, or software licenses) directly through the platform. You approve them, and the payout and receipt are publicly recorded.

Platform Integration: You can link your Open Collective account to your existing funding streams, meaning funds coming in from GitHub Sponsors or Patreon can be tracked in one unified dashboard.

2. The Native Route: A Transparency Repository
If you prefer a DIY approach that keeps everything within your existing ecosystem, you can use a dedicated GitHub repository (e.g., [github.com/PoseStudio/transparency](https://github.com/PoseStudio/transparency)).

The Ledger: Maintain a FINANCES.md file or a public Google Sheet linked in the repository. Update it on the first of every month with your starting balance, itemized income, and itemized expenses.

Receipt Tracking: Upload redacted receipts (removing personal banking details) for infrastructure costs or asset purchases directly into the repository so sponsors can verify the expenses.

Clear Policies: Use the README in this repository to explicitly state your financial policies. For example, clarify how much of the budget goes toward infrastructure versus compensating developer time.

3. The Narrative Route: Periodic Transparency Reports
Raw numbers and spreadsheets offer transparency, but they don't always provide context. Pairing your ledger with a narrative helps sponsors understand the impact of their money.

Monthly Updates: Publish a monthly "Transparency Report" in your GitHub Discussions, on your Patreon, or on your project website.

Highlight ROI: Break down what the funding actually achieved. Instead of just listing "$50 for hosting," phrase it as "Sponsor funds covered our web server security and routing costs this month, keeping the site fast and secure."

Future Roadmaps: Detail exactly what future funds will be saved for. If you need to hit a certain milestone to afford a new plugin, a dedicated server, or to justify spending 20 more hours a week on coding, make that goal public.

For maximum transparency with minimal ongoing administrative work, routing your finances through Open Collective is usually the most sustainable long-term solution.

Want to explore fiscal hosts?

Yes

can you create a comprehensive example of a FINANCES.md that we can use as a template.

Your MD file is ready
MD icon
 FINANCES 
MD

This template is structured to handle everything a sponsor wants to see: clear income tracking from your Patreon and GitHub Sponsors, categorized expense logs for things like your PoseStudio.org domain and AWS WAF overhead, and a clear set of project priorities. You can commit this directly to your GitHub repository and update the tables at the end of each month.




Gemini is AI and can make mistakes.

# PoseStudio Financial Transparency Ledger

Welcome to the official financial transparency document for **PoseStudio**. 

We believe that open source means open operations. Because this project is entirely community-funded, this document serves to provide our sponsors and contributors with 100% visibility into how much money is received and exactly where every dollar is spent.

## 💰 Current Funding Sources
Our development, infrastructure, and operations are supported by our community. You can support the development of PoseStudio through:
* **[GitHub Sponsors](https://github.com/sponsors/PoseStudio)** * **[Patreon](https://www.patreon.com/PoseStudio)**

---

## 📊 Financial Summary (Year-to-Date)
* **Starting Balance:** $0.00
* **Total Income:** $0.00
* **Total Expenses:** $0.00
* **Current Reserve:** $0.00

---

## 🧾 Monthly Ledger

### [Current Month/Year, e.g., May 2026]

#### ⬇️ Income (Inflow)
| Source | Amount | Notes |
| :--- | :--- | :--- |
| GitHub Sponsors | $0.00 | Monthly recurring payouts |
| Patreon | $0.00 | Monthly recurring payouts |
| One-time Donations | $0.00 | Direct contributions |
| **Total Income** | **$0.00** | |

#### ⬆️ Expenses (Outflow)
| Category | Item | Amount | Receipt / Proof | Notes |
| :--- | :--- | :--- | :--- | :--- |
| Infrastructure | Domain Registration | $0.00 | [Link to receipt] | PoseStudio.org (Annual) |
| Infrastructure | Web Hosting & Security | $0.00 | [Link to receipt] | Server costs, AWS WAF, bandwidth |
| Development | Software Licenses | $0.00 | [Link to receipt] | Essential dev tools/IDEs |
| Compensation | Developer Time | $0.00 | N/A | Sustaining daily development |
| **Total Expenses** | | **$0.00** | | |

*Historical ledgers for previous months are archived in the `financial-archives/` folder.*

---

## 📋 Expense Policy & Priorities

As the project grows and funding increases, funds will be allocated strictly according to the following priorities to ensure the long-term sustainability of PoseStudio as a free, open-source 3D character platform:

1. **Core Infrastructure:** Keeping our websites online, securing our servers against bots/malicious traffic, and paying for repository hosting or CI/CD pipelines.
2. **Essential Tools & Assets:** Covering the costs of software licenses, documentation tools, or 3D asset reference materials required for development.
3. **Developer Compensation:** Compensating the active development team so they can dedicate consistent, focused hours to the project rather than treating it solely as a weekend hobby. 
4. **Community Bounties:** Funding specific community pull requests, bug fixes, or highly-requested features.

## 🔍 Receipt Verification & Auditing
To ensure absolute trust, redacted receipts (with personal banking and home address information removed) for all non-payroll expenses are stored locally in the `receipts/` directory of this repository.

---
*This ledger is updated by the project maintainers on the 1st of every month. If you have any questions or concerns regarding our financial allocation, please open a thread in our GitHub Discussions.*
FINANCES.md
Displaying FINANCES.md.
