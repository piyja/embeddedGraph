---
name: market-analysis
description: Run a market analysis of tech stocks, renewable energy, energy storage, AI infrastructure, German XETRA stocks, Indian ETFs, and Indian mutual funds. Use when asked to analyze markets, check stock ratings, research ethical investing, find buy opportunities, or assess company fundamentals. Covers buying rating (1-5) and ethical rating (1-5). Avoids defense, oil & gas, fast food, tobacco, sugary drinks.
---

Market analysis tool driven by `.claude/skills/market-analysis/driver.py`.
Data sources: Yahoo Finance (global stocks/ETFs) + mfapi.in (Indian mutual fund NAVs).
Output: colored sectioned tables — **Global Markets** (Tech, Renewables, Energy Storage, AI Infra) and **Indian Markets** (NSE ETFs + Mutual Funds Direct Growth).

All paths below are relative to `/home/piyushdj/personal/embeddedGraph/`.

## Prerequisites

```bash
python3 -m venv /tmp/market-venv
/tmp/market-venv/bin/pip install yfinance tabulate requests rich
/tmp/market-venv/bin/python3 -c "import yfinance, rich; print('ok')"
# → ok
```

The venv at `/tmp/market-venv` is expected to already exist from the first session. If it's gone (container restart), re-run the above.

## Run (agent path)

### Full analysis — both sections

```bash
cd /home/piyushdj/personal/embeddedGraph
/tmp/market-venv/bin/python3 .claude/skills/market-analysis/driver.py
```

### Global markets only (Tech + Renewables + Storage + AI Infra)

```bash
/tmp/market-venv/bin/python3 .claude/skills/market-analysis/driver.py --section global
```

### Individual global sub-sections

```bash
/tmp/market-venv/bin/python3 .claude/skills/market-analysis/driver.py --section tech
/tmp/market-venv/bin/python3 .claude/skills/market-analysis/driver.py --section renewable
/tmp/market-venv/bin/python3 .claude/skills/market-analysis/driver.py --section storage
/tmp/market-venv/bin/python3 .claude/skills/market-analysis/driver.py --section ai-infra
```

### Indian markets only (NSE ETFs + Mutual Funds)

```bash
/tmp/market-venv/bin/python3 .claude/skills/market-analysis/driver.py --section india
```

### Verbose mode — CEO, employees, D/E ratio, ROE, 52W range, summary

```bash
/tmp/market-venv/bin/python3 .claude/skills/market-analysis/driver.py --section tech --verbose
```

### Custom tickers

```bash
/tmp/market-venv/bin/python3 .claude/skills/market-analysis/driver.py --tickers NVDA SAP.DE FSLR NIFTYBEES.NS
```

Ticker format reference:
| Market | Format | Examples |
|---|---|---|
| German XETRA | `TICKER.DE` | `SAP.DE`, `IFX.DE`, `ENR.DE`, `NVD.DE` |
| Copenhagen | `TICKER.CO` | `ORSTED.CO`, `VWS.CO` |
| Amsterdam | `TICKER.AS` | `ASML.AS` |
| NSE India ETF | `TICKER.NS` | `NIFTYBEES.NS`, `ITETF.NS` |
| US | no suffix | `NVDA`, `FSLR`, `MSFT` |
| HK | `TICKER.HK` | `1211.HK` (BYD) |

### JSON output

```bash
/tmp/market-venv/bin/python3 .claude/skills/market-analysis/driver.py --section global --format json
```

## Curated watchlists (built-in)

**Tech:** SAP.DE, IFX.DE, ASML.AS, MSFT, NVDA, TSM, CDNS, SNPS, AMD

**Renewable Energy:** VWS.CO (Vestas), ORSTED.CO (Orsted), FSLR (First Solar), ENR.DE (Siemens Energy), S92.DE (SMA Solar), NDX1.DE (Nordex), ENPH (Enphase)

**Energy Storage:** FLNC (Fluence), TSLA (Tesla Megapack), ENVX (Enovix), BYDDY (BYD), ETN (Eaton), QS (QuantumScape)

**AI Infrastructure:** VRT (Vertiv), ANET (Arista Networks), EQIX (Equinix), ETN (Eaton), NVDA, AMD

**India ETFs (NSE):** NIFTYBEES.NS, ITETF.NS, ITIETF.NS, SETFNN50.NS, MAFANG.NS, HDFCNIFTY.NS

**India Mutual Funds (Direct Growth, via mfapi.in):**

| Fund | Scheme Code | Category |
|---|---|---|
| Mirae Asset AI & Tech ETF FoF | 150597 | Tech / AI |
| ICICI Pru Technology Fund | 120594 | Tech |
| SBI Technology Opportunities | 120578 | Tech |
| Franklin India Technology Fund | 118537 | Tech |
| Tata Digital India Fund | 135800 | Tech |
| DSP Global Clean Energy FoF | 119275 | Renewable |
| DSP Natural Resources & New Energy | 119028 | Renewable |
| Kotak ESG Exclusionary Fund | 148606 | ESG |
| Mirae Asset Nifty 100 ESG FoF | 148574 | ESG |

## Rating logic

### Buying rate (1–5)

| Score | Meaning |
|---|---|
| 5 | Strong buy — multiple positive signals firing |
| 4 | Good entry — 2 positive signals |
| 3 | Neutral — hold or watch |
| 2 | Caution — declining revenue or unprofitable |
| 1 | Avoid — multiple red flags |

Global stock signals: 52W position (<30% → +1, >88% → −1), forward P/E (<15x → +1, >60x → −1), revenue growth (>25% → +1, negative → −1), profit margin (>20% → +1, negative → −1), debt-to-equity (>150 → −1).

Indian MF signals: 1Y return (>30% → +1, <−10% → −1), 3M momentum (>5% → +1, <−5% → −1). Base score 3.

### Ethical rate (1–5)

| Score | Meaning |
|---|---|
| 5 | Strongly ethical — clean tech, renewables, pure software, ESG-screened |
| 4 | Good — general tech, healthcare |
| 3 | Neutral — mixed conglomerate or unknown |
| 2 | Moderate concern — airlines, chemicals, brewers |
| 1 | Hard avoid — defense, fossil fuel, fast food, tobacco, sugary drinks |

**Permanently scored 1/5 (avoid):**
- Defense: `LMT`, `RTX`, `NOC`, `GD`, `BA`, `LDOS`
- Oil & Gas: `XOM`, `CVX`, `BP`, `SHEL`, `TTE`, `COP`, `OXY`
- Fast Food: `MCD`, `YUM`, `QSR`, `DPZ`, `WEN`
- Tobacco: `PM`, `MO`, `BTI`
- Sugary drinks: `KO`, `PEP`

## Deep research workflow (agent steps)

After running the driver, for 4★+ picks:

1. **News** — `WebSearch("[TICKER] news June 2026")`
2. **Balance sheet deep-dive** — `WebSearch("[company] Q1 2026 earnings revenue profit margin")`
3. **Leadership/values** — `WebSearch("[CEO name] interview ESG 2025 2026")`
4. **German access** — For US stocks: check `NVD.DE` (NVIDIA), `F3A.DE` (First Solar), `MSF.DE` (Microsoft), `ASML.DE` (ASML). Most major US names have a Frankfurt listing.
5. **India MF NAV** — `WebFetch("http://api.mfapi.in/mf/[schemeCode]")` — returns JSON with daily NAV history.

## Gotchas

- **Encavis (ECV.DE) delisted** — taken private, returns no Yahoo Finance data. Removed from watchlist.
- **BYD.DE not available** — use `BYDDY` (US OTC) or `1211.HK` (Hong Kong). The `.DE` listing doesn't have Yahoo Finance data.
- **mfapi.in requires HTTP not HTTPS** — the driver uses `http://api.mfapi.in/...` to avoid SSL timeouts in this container.
- **Indian IT ETFs (ITETF.NS, ITIETF.NS) show no sector metadata** — yfinance returns blank country/sector for most NSE ETFs. Ethical rating defaults to 3. Indian IT index (Nifty IT) tracks TCS, Infosys, Wipro, HCLTech, Tech Mahindra — manually rate 4/5 ethics.
- **Siemens Energy (ENR.DE)** includes Siemens Gamesa wind but also gas turbines — ethics 3/5, not 5/5.
- **Tesla (TSLA) ethics** — rated 3/5 (Elon Musk controversy, labor concerns) despite the product being clean energy.
- **Rate limiting** — Yahoo Finance throttles bulk requests. If you get HTTP 429, wait 30s and retry or split into smaller `--tickers` batches.

## Troubleshooting

- **`ModuleNotFoundError`**: Use `/tmp/market-venv/bin/python3`, not system `python3`.
- **`HTTP 404` for ticker**: Wrong suffix — verify at `finance.yahoo.com/quote/[TICKER]`.
- **`HTTP 429`**: Yahoo rate-limited. Wait 30s, reduce batch size.
- **mfapi timeout**: The container occasionally times out on HTTPS to mfapi.in — driver uses HTTP which works.
- **`No data returned` for ETF**: Try the `.NS` suffix variant or check if the fund changed its ticker on NSE.
