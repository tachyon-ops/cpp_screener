import { createBot, createProvider, createFlow, addKeyword, MemoryDB } from '@builderbot/bot';
import { BaileysProvider } from '@builderbot/provider-baileys';
import { fetchLatestBaileysVersion } from 'baileys';
import { WebSocket } from 'ws';
import dotenv from 'dotenv';
import path from 'path';
import { fileURLToPath } from 'url';
const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
dotenv.config({ path: path.resolve(__dirname, '../secrets.env') });
const BACKEND_URL = process.env.BACKEND_URL || 'http://localhost:8080';
const WS_URL = process.env.WS_URL || 'ws://localhost:8080/ws/live';
const PORT = parseInt(process.env.PORT || '3000', 10);
console.log(`[Config] BACKEND_URL=${BACKEND_URL}`);
console.log(`[Config] WS_URL=${WS_URL}`);
console.log(`[Config] PORT=${PORT}`);
// Helper to verify if the sender is authorized based on database settings
async function isAuthorized(from) {
    try {
        const res = await fetch(`${BACKEND_URL}/api/settings`);
        if (!res.ok) {
            console.error(`[Auth] Failed to retrieve settings: ${res.statusText}`);
            return false;
        }
        const settings = await res.json();
        if (settings.whatsapp_enabled !== 'true') {
            console.log(`[Auth] WhatsApp notifications are disabled in settings`);
            return false;
        }
        const recipient = settings.whatsapp_recipient || '';
        if (!recipient) {
            console.log(`[Auth] No recipient phone number configured`);
            return false;
        }
        const cleanFrom = from.replace(/\D/g, '');
        const cleanRecipient = recipient.replace(/\D/g, '');
        const authorized = cleanFrom === cleanRecipient && cleanRecipient.length > 0;
        if (!authorized) {
            console.log(`[Auth] Unauthorized message from ${from}. Recipient set: ${recipient}`);
        }
        return authorized;
    }
    catch (e) {
        console.error(`[Auth] Error validating sender:`, e.message);
        return false;
    }
}
// Unified help text (shared between /help and /start)
const HELP_TEXT = [
    '🤖 *Tachyon Screener Bot Commands* 🤖',
    '',
    '*Data Queries:*',
    '• `/regime` — Current market regime',
    '• `/candidates` — List active swing setups',
    '• `/alerts` — List recent signals',
    '',
    '*Subscriptions:*',
    '• `/set_premium` — Subscribe to Premium alerts',
    '• `/set_opportunity` — Subscribe to Opportunity alerts',
    '• `/set_digest` — Subscribe to Digest alerts',
    '• `/unset_premium` — Remove Premium subscription',
    '• `/unset_opportunity` — Remove Opportunity subscription',
    '• `/unset_digest` — Remove Digest subscription',
    '• `/stop` — Unsubscribe from all alerts',
    '',
    '*System:*',
    '• `/status` — Show engine status & stats',
    '• `/act <id>` — Execute trade for alert',
    '• `/skip <id>` — Dismiss alert',
    '• `/help` — Show this help menu'
].join('\n');
// Bot flows
const helpFlow = addKeyword(['/help', 'help', 'menu'])
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    await flowDynamic(HELP_TEXT);
});
const startFlow = addKeyword(['/start'])
    .addAction(async (ctx, { flowDynamic }) => {
    // /start auto-subscribes all tiers (same as Telegram)
    try {
        await fetch(`${BACKEND_URL}/api/settings`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                whatsapp_enabled: 'true',
                whatsapp_recipient: ctx.from,
                wa_tier_premium: 'true',
                wa_tier_opportunity: 'true',
                wa_tier_digest: 'true'
            })
        });
    }
    catch (e) { }
    await flowDynamic([
        '⚡ *Tachyon Trading Screener* ⚡',
        '',
        '✅ *Registration complete!*',
        'This number is now subscribed to all alert tiers:',
        '  🟢 Premium  •  🟡 Opportunity  •  ⚪ Digest',
        '',
        HELP_TEXT
    ].join('\n'));
});
const statusFlow = addKeyword(['/status'])
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    try {
        const [settingsRes, regimeRes, candidatesRes, positionsRes] = await Promise.all([
            fetch(`${BACKEND_URL}/api/settings`),
            fetch(`${BACKEND_URL}/api/regime`),
            fetch(`${BACKEND_URL}/api/candidates`),
            fetch(`${BACKEND_URL}/api/positions`)
        ]);
        const settings = settingsRes.ok ? await settingsRes.json() : {};
        const regime = regimeRes.ok ? await regimeRes.json() : [];
        const candidates = candidatesRes.ok ? (await candidatesRes.json()).filter((c) => c.status === 'active') : [];
        const positions = positionsRes.ok ? await positionsRes.json() : [];
        const lines = [
            '📊 *Tachyon Status*',
            '',
            `Regime: *${regime[0]?.regime?.toUpperCase() || 'Unknown'}*`,
            `Active Candidates: ${candidates.length}`,
            `Open Positions: ${positions.length}`,
            '',
            '*WhatsApp Subscriptions:*',
            `  🟢 Premium: ${settings.wa_tier_premium === 'true' ? '✅' : '❌'}`,
            `  🟡 Opportunity: ${settings.wa_tier_opportunity === 'true' ? '✅' : '❌'}`,
            `  ⚪ Digest: ${settings.wa_tier_digest === 'true' ? '✅' : '❌'}`
        ];
        await flowDynamic(lines.join('\n'));
    }
    catch (e) {
        await flowDynamic(`❌ Error: ${e.message}`);
    }
});
// Topic subscription flows (mirrors Telegram's /set_premium, /set_opportunity, /set_digest)
const setPremiumFlow = addKeyword(['/set_premium'])
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    try {
        await fetch(`${BACKEND_URL}/api/settings`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ wa_tier_premium: 'true' })
        });
        await flowDynamic('✅ Subscribed to 🟢 *PREMIUM* alerts.');
    }
    catch (e) {
        await flowDynamic(`❌ Error: ${e.message}`);
    }
});
const setOpportunityFlow = addKeyword(['/set_opportunity'])
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    try {
        await fetch(`${BACKEND_URL}/api/settings`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ wa_tier_opportunity: 'true' })
        });
        await flowDynamic('✅ Subscribed to 🟡 *OPPORTUNITY* alerts.');
    }
    catch (e) {
        await flowDynamic(`❌ Error: ${e.message}`);
    }
});
const setDigestFlow = addKeyword(['/set_digest'])
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    try {
        await fetch(`${BACKEND_URL}/api/settings`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ wa_tier_digest: 'true' })
        });
        await flowDynamic('✅ Subscribed to ⚪ *DIGEST* alerts.');
    }
    catch (e) {
        await flowDynamic(`❌ Error: ${e.message}`);
    }
});
// Topic unsubscription flows (mirrors Telegram's /unset_premium, /unset_opportunity, /unset_digest)
const unsetPremiumFlow = addKeyword(['/unset_premium'])
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    try {
        await fetch(`${BACKEND_URL}/api/settings`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ wa_tier_premium: 'false' })
        });
        await flowDynamic('🟢 *PREMIUM* alerts have been unsubscribed.');
    }
    catch (e) {
        await flowDynamic(`❌ Error: ${e.message}`);
    }
});
const unsetOpportunityFlow = addKeyword(['/unset_opportunity'])
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    try {
        await fetch(`${BACKEND_URL}/api/settings`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ wa_tier_opportunity: 'false' })
        });
        await flowDynamic('🟡 *OPPORTUNITY* alerts have been unsubscribed.');
    }
    catch (e) {
        await flowDynamic(`❌ Error: ${e.message}`);
    }
});
const unsetDigestFlow = addKeyword(['/unset_digest'])
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    try {
        await fetch(`${BACKEND_URL}/api/settings`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ wa_tier_digest: 'false' })
        });
        await flowDynamic('⚪ *DIGEST* alerts have been unsubscribed.');
    }
    catch (e) {
        await flowDynamic(`❌ Error: ${e.message}`);
    }
});
const regimeFlow = addKeyword(['/regime'])
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    try {
        const res = await fetch(`${BACKEND_URL}/api/regime`);
        if (res.ok) {
            const data = await res.json();
            if (data && data.length > 0) {
                const r = data[0];
                await flowDynamic([
                    `📊 *Current Market Regime*`,
                    `• *Regime*: ${r.regime?.toUpperCase()}`,
                    `• *SPY 50MA*: ${r.spy_above_50 ? 'Above' : 'Below'}`,
                    `• *SPY 200MA*: ${r.spy_above_200 ? 'Above' : 'Below'}`,
                    `• *VIX*: ${r.vix?.toFixed(2)}`,
                    `• *TS*: ${r.ts}`
                ].join('\n'));
            }
            else {
                await flowDynamic('No regime data available.');
            }
        }
        else {
            await flowDynamic(`Error fetching regime: ${res.statusText}`);
        }
    }
    catch (e) {
        await flowDynamic(`Failed to query engine: ${e.message}`);
    }
});
const candidatesFlow = addKeyword(['/candidates'])
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    try {
        const res = await fetch(`${BACKEND_URL}/api/candidates`);
        if (res.ok) {
            const candidates = await res.json();
            const active = candidates.filter(c => c.status === 'active');
            if (active.length === 0) {
                await flowDynamic('🎯 No active swing setups in watchlist.');
                return;
            }
            const lines = [
                `🎯 *Active Swing Watchlist (${active.length})*`,
                ''
            ];
            active.forEach(c => {
                lines.push(`• *${c.symbol}* (${c.name || 'Unknown'})`);
                lines.push(`  - Screen: Class ${c.screen}`);
                lines.push(`  - Entry Zone: $${c.entry_zone_low} - $${c.entry_zone_high}`);
                lines.push(`  - Stop: $${c.suggested_stop} | R:R: ${c.rr_target}`);
                if (c.notes)
                    lines.push(`  - Notes: ${c.notes}`);
                lines.push('');
            });
            await flowDynamic(lines.join('\n'));
        }
        else {
            await flowDynamic(`Error fetching candidates: ${res.statusText}`);
        }
    }
    catch (e) {
        await flowDynamic(`Failed to query engine: ${e.message}`);
    }
});
const alertsFlow = addKeyword(['/alerts'])
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    try {
        const res = await fetch(`${BACKEND_URL}/api/alerts`);
        if (res.ok) {
            const alerts = await res.json();
            if (alerts.length === 0) {
                await flowDynamic('🚨 No screener alerts recorded.');
                return;
            }
            const recent = alerts.slice(0, 5);
            const lines = [
                `🚨 *Recent Screener Alerts (Top 5)*`,
                ''
            ];
            recent.forEach(a => {
                lines.push(`• *ID ${a.id}* - ${a.payload?.symbol || 'Unknown'} (${a.ts})`);
                lines.push(`  - Screen: Class ${a.screen} | Price: $${a.payload?.price || 'N/A'}`);
                lines.push(`  - Regime: ${a.regime_at_alert} | Acted: ${a.acted_on ? 'Yes' : 'No'}`);
                lines.push('');
            });
            await flowDynamic(lines.join('\n'));
        }
        else {
            await flowDynamic(`Error fetching alerts: ${res.statusText}`);
        }
    }
    catch (e) {
        await flowDynamic(`Failed to query engine: ${e.message}`);
    }
});
const actFlow = addKeyword('/^\\\\/act\\\\s+(\\\\d+)$/', { regex: true })
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    const match = ctx.body.match(/^\/act\s+(\d+)$/);
    if (!match)
        return;
    const alertId = parseInt(match[1], 10);
    try {
        const res = await fetch(`${BACKEND_URL}/api/alert_response`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ alert_id: alertId, action: 'execute' })
        });
        if (res.ok) {
            await flowDynamic(`✅ Executed simulated trade for alert *ID ${alertId}*.`);
        }
        else {
            try {
                const errData = await res.json();
                await flowDynamic(`❌ Error: ${errData.error || res.statusText}`);
            }
            catch {
                await flowDynamic(`❌ Error: ${res.statusText}`);
            }
        }
    }
    catch (e) {
        await flowDynamic(`Failed to execute alert response: ${e.message}`);
    }
});
const skipFlow = addKeyword('/^\\\\/skip\\\\s+(\\\\d+)$/', { regex: true })
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    const match = ctx.body.match(/^\/skip\s+(\d+)$/);
    if (!match)
        return;
    const alertId = parseInt(match[1], 10);
    try {
        const res = await fetch(`${BACKEND_URL}/api/alert_response`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ alert_id: alertId, action: 'dismiss' })
        });
        if (res.ok) {
            await flowDynamic(`✅ Dismissed alert *ID ${alertId}*.`);
        }
        else {
            try {
                const errData = await res.json();
                await flowDynamic(`❌ Error: ${errData.error || res.statusText}`);
            }
            catch {
                await flowDynamic(`❌ Error: ${res.statusText}`);
            }
        }
    }
    catch (e) {
        await flowDynamic(`Failed to dismiss alert response: ${e.message}`);
    }
});
const unsubscribeFlow = addKeyword(['/stop', '/unsubscribe', 'unsubscribe'])
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    try {
        const res = await fetch(`${BACKEND_URL}/api/settings`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                whatsapp_enabled: 'false',
                whatsapp_recipient: '',
                wa_tier_premium: 'false',
                wa_tier_opportunity: 'false',
                wa_tier_digest: 'false'
            })
        });
        if (res.ok) {
            await flowDynamic('❌ *WhatsApp Notifications Disabled*\n\nAll alert subscriptions have been removed and notifications are turned off.\nSend /start to re-enable.');
        }
        else {
            await flowDynamic('❌ *Error*: Failed to disable notifications in system settings.');
        }
    }
    catch (e) {
        await flowDynamic(`❌ *Error*: Failed to query engine: ${e.message}`);
    }
});
// Programmatic message dispatch helper
async function sendWhatsAppMessage(botInstance, provider, jid, text) {
    try {
        const isConnected = !!(provider?.vendor?.user || botInstance?.provider?.vendor?.user);
        if (!isConnected) {
            console.log(`[WS] Bypassing message transmission: WhatsApp provider is not connected/linked yet.`);
            return;
        }
        if (botInstance.sendMessage) {
            await botInstance.sendMessage(jid, text, {});
        }
        else if (botInstance.provider && botInstance.provider.sendMessage) {
            await botInstance.provider.sendMessage(jid, text, {});
        }
        else if (provider.sendMessage) {
            await provider.sendMessage(jid, text, {});
        }
        else {
            console.error('[WS] Could not find sendMessage method on bot or provider.');
        }
    }
    catch (err) {
        console.error('[WS] Error sending message:', err.message);
    }
}
// WebSocket Listener for trading engine feed
let ws = null;
function connectWebSocket(botInstance, provider) {
    console.log(`[WS] Connecting to live feed at ${WS_URL}...`);
    ws = new WebSocket(WS_URL);
    ws.on('open', () => {
        console.log(`[WS] WebSocket connection established successfully.`);
    });
    ws.on('message', async (data) => {
        try {
            const message = JSON.parse(data.toString());
            if (message.type === 'alert') {
                const alert = message.data;
                console.log(`[WS] Received live alert from C++ engine:`, alert);
                const res = await fetch(`${BACKEND_URL}/api/settings`);
                if (!res.ok) {
                    console.error(`[WS] Failed to fetch settings: ${res.statusText}`);
                    return;
                }
                const settings = await res.json();
                if (settings.whatsapp_enabled !== 'true') {
                    console.log(`[WS] Alert bypassed: WhatsApp is disabled in UI`);
                    return;
                }
                // Check tier-level subscription (mirrors Telegram's per-tier routing)
                const tierMap = {
                    'premium': 'wa_tier_premium',
                    'opportunity': 'wa_tier_opportunity',
                    'interesting': 'wa_tier_digest',
                    'digest': 'wa_tier_digest'
                };
                const tierKey = tierMap[alert.tier] || '';
                if (tierKey && settings[tierKey] === 'false') {
                    console.log(`[WS] Alert bypassed: tier '${alert.tier}' is unsubscribed (${tierKey}=${settings[tierKey]})`);
                    return;
                }
                const recipient = settings.whatsapp_recipient || '';
                if (!recipient) {
                    console.log(`[WS] Alert bypassed: No recipient phone number set in UI`);
                    return;
                }
                const cleanRecipient = recipient.replace(/\D/g, '');
                const jid = `${cleanRecipient}@s.whatsapp.net`;
                const text = [
                    `🚨 *TACHYON ALERT TRIGGERED* 🚨`,
                    ``,
                    `• *ID*: ${alert.id}`,
                    `• *Time*: ${alert.ts}`,
                    `• *Screen*: Screen ${alert.screen}`,
                    `• *Symbol*: ${alert.payload?.symbol || 'Unknown'}`,
                    `• *Price*: $${alert.payload?.price || '0.00'}`,
                    `• *Regime*: ${alert.regime_at_alert || 'Unknown'}`,
                    `• *Tier*: Tier ${alert.tier}`,
                    ``,
                    `Reply with:`,
                    `• \`/act ${alert.id}\` to execute simulated trade`,
                    `• \`/skip ${alert.id}\` to dismiss alert`
                ].join('\n');
                console.log(`[WS] Forwarding alert ${alert.id} to ${jid}`);
                await sendWhatsAppMessage(botInstance, provider, jid, text);
            }
            else if (message.type === 'test_whatsapp') {
                const testData = message.data;
                console.log(`[WS] Received live test alert from C++ engine:`, testData);
                const recipient = testData.whatsapp_recipient || '';
                if (!recipient) {
                    console.log(`[WS] Test alert bypassed: No recipient phone number configured`);
                    return;
                }
                const cleanRecipient = recipient.replace(/\D/g, '');
                const jid = `${cleanRecipient}@s.whatsapp.net`;
                const text = testData.text || `⚡ *Tachyon Screener* ⚡\n🤖 *WhatsApp Bot Test Connection*\n✅ Connection verified successfully!`;
                console.log(`[WS] Forwarding test alert to ${jid}`);
                await sendWhatsAppMessage(botInstance, provider, jid, text);
            }
        }
        catch (e) {
            console.error(`[WS] Failed to handle WebSocket message:`, e.message);
        }
    });
    ws.on('close', () => {
        console.log(`[WS] WebSocket connection closed. Reconnecting in 5 seconds...`);
        setTimeout(() => connectWebSocket(botInstance, provider), 5000);
    });
    ws.on('error', (err) => {
        console.error(`[WS] WebSocket error:`, err.message);
    });
}
// Report WhatsApp bot status to C++ backend
async function reportStatus(state, pairingCode, errorMessage) {
    try {
        const body = { state };
        if (pairingCode)
            body.pairing_code = pairingCode;
        if (errorMessage)
            body.error_message = errorMessage;
        const res = await fetch(`${BACKEND_URL}/api/whatsapp_status`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        });
        if (!res.ok) {
            console.error(`[Status] Failed to report status '${state}': ${res.statusText}`);
        }
        else {
            console.log(`[Status] Reported status: ${state}${pairingCode ? ` (code: ${pairingCode})` : ''}`);
        }
    }
    catch (e) {
        console.error(`[Status] Error reporting status '${state}':`, e.message);
    }
}
// Main execution block
const main = async () => {
    let version = [2, 3000, 1035194821];
    try {
        const { version: latestVersion, isLatest } = await fetchLatestBaileysVersion();
        console.log(`[Bot] Fetched latest Baileys WhatsApp Web version: ${latestVersion.join('.')}. Is latest: ${isLatest}`);
        version = latestVersion;
    }
    catch (e) {
        console.error(`[Bot] Failed to fetch latest Baileys version, using fallback:`, e.message);
    }
    // Determine phone number for pairing code fallback
    let pairingPhoneNumber = process.env.WHATSAPP_PHONE_NUMBER;
    if (!pairingPhoneNumber) {
        try {
            const res = await fetch(`${BACKEND_URL}/api/settings`);
            if (res.ok) {
                const settings = await res.json();
                if (settings.whatsapp_enabled === 'true' && settings.whatsapp_recipient) {
                    pairingPhoneNumber = settings.whatsapp_recipient.replace(/\D/g, '');
                    console.log(`[Bot] No explicit WHATSAPP_PHONE_NUMBER env var found. Using database recipient number for pairing: ${pairingPhoneNumber}`);
                }
            }
        }
        catch (e) {
            console.error(`[Bot] Could not fetch settings from C++ engine for pairing number lookup:`, e.message);
        }
    }
    const providerOptions = { version };
    if (pairingPhoneNumber) {
        providerOptions.usePairingCode = true;
        providerOptions.phoneNumber = pairingPhoneNumber;
        console.log(`[Bot] Pairing code mode configured with phone number: ${pairingPhoneNumber}`);
    }
    else {
        console.log(`[Bot] QR code scan mode configured (no pairing phone number available).`);
    }
    const adapterFlow = createFlow([
        startFlow, helpFlow, statusFlow,
        regimeFlow, candidatesFlow, alertsFlow, actFlow, skipFlow,
        setPremiumFlow, setOpportunityFlow, setDigestFlow,
        unsetPremiumFlow, unsetOpportunityFlow, unsetDigestFlow,
        unsubscribeFlow
    ]);
    const adapterProvider = createProvider(BaileysProvider, providerOptions);
    const adapterDB = new MemoryDB();
    console.log('[Bot] Starting BuilderBot service...');
    const bot = await createBot({
        flow: adapterFlow,
        provider: adapterProvider,
        database: adapterDB,
    });
    // Listen for Baileys provider events and report status to C++ backend
    adapterProvider.on('require_action', (payload) => {
        const code = payload?.payload?.code || '';
        if (code) {
            console.log(`[Status] Pairing code received: ${code}`);
            reportStatus('awaiting_pairing', code);
        }
    });
    adapterProvider.on('ready', () => {
        console.log(`[Status] WhatsApp provider connected.`);
        reportStatus('connected');
    });
    adapterProvider.on('auth_failure', (payload) => {
        const msg = Array.isArray(payload) ? payload.join('; ') : String(payload);
        console.error(`[Status] WhatsApp auth failure: ${msg}`);
        reportStatus('error', undefined, msg);
    });
    // Start listening to live WebSocket alerts
    connectWebSocket(bot, adapterProvider);
    // Start local server to keep Baileys instance alive and handle webhooks
    if (bot.httpServer) {
        bot.httpServer(PORT);
        console.log(`[Bot] HTTP webhook server listening on port ${PORT}`);
    }
    else {
        console.log(`[Bot] Warning: bot.httpServer is not defined, skipping webhook initialization.`);
    }
};
main().catch((err) => {
    console.error('[Bot] Fatal initialization error:', err);
});
