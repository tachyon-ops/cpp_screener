import { createBot, createProvider, createFlow, addKeyword, MemoryDB } from '@builderbot/bot';
import { BaileysProvider } from '@builderbot/provider-baileys';
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
// Bot flows
const helpFlow = addKeyword(['/help', 'help', 'menu'])
    .addAction(async (ctx, { flowDynamic }) => {
    if (!(await isAuthorized(ctx.from)))
        return;
    await flowDynamic([
        '🤖 *Tachyon Screener Bot Commands* 🤖',
        '',
        '• `/regime` : Current market regime',
        '• `/candidates` : List active swing setups',
        '• `/alerts` : List recent screener signals',
        '• `/act <id>` : Mark alert as acted-on / execute trade',
        '• `/skip <id>` : Mark alert as skipped / dismiss',
        '• `/help` : Show this help message'
    ].join('\n'));
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
// Programmatic message dispatch helper
async function sendWhatsAppMessage(botInstance, provider, jid, text) {
    try {
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
// Main execution block
const main = async () => {
    const adapterFlow = createFlow([helpFlow, regimeFlow, candidatesFlow, alertsFlow, actFlow, skipFlow]);
    const adapterProvider = createProvider(BaileysProvider);
    const adapterDB = new MemoryDB();
    console.log('[Bot] Starting BuilderBot service...');
    const bot = await createBot({
        flow: adapterFlow,
        provider: adapterProvider,
        database: adapterDB,
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
