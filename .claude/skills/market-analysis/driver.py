#!/usr/bin/env python3
"""
Market Analysis Driver v3
Fetches live data from Yahoo Finance (global stocks) + mfapi.in (Indian MFs).
Outputs sectioned, colored tables + daily Top 5 Buy / Top 5 Sell signals.

Usage:
  python3 driver.py                        # all sections
  python3 driver.py --section global       # tech + renewable + storage + ai-infra
  python3 driver.py --section india        # Indian ETFs + mutual funds
  python3 driver.py --section portfolio    # personal holdings with daily signals
  python3 driver.py --section tech
  python3 driver.py --section renewable
  python3 driver.py --section storage
  python3 driver.py --section ai-infra
  python3 driver.py --tickers NVDA SAP.DE FSLR
  python3 driver.py --format json
"""

import sys
import json
import time
import argparse
from datetime import datetime

try:
    import yfinance as yf
    import requests
    from rich.console import Console
    from rich.table import Table
    from rich.panel import Panel
    from rich.text import Text
    from rich import box
    from rich.columns import Columns
    from rich.rule import Rule
except ImportError:
    print("ERROR: Missing dependencies. Run:")
    print("  python3 -m venv /tmp/market-venv")
    print("  /tmp/market-venv/bin/pip install yfinance tabulate requests rich")
    sys.exit(1)

console = Console()

# ── Ethical screening ────────────────────────────────────────────────────────
UNETHICAL_INDUSTRIES = {
    "Aerospace & Defense",
    "Defense Contractors",
    "Oil & Gas Integrated",
    "Oil & Gas E&P",
    "Oil & Gas Midstream",
    "Oil & Gas Refining & Marketing",
    "Oil & Gas Equipment & Services",
    "Tobacco",
    "Gambling",
    "Adult Entertainment",
    "Weapons & Ammunition",
    "Restaurants",            # Fast food
    "Fast Food",
}

AVOID_TICKERS = {
    # Explicitly avoid — fast food
    "MCD", "YUM", "QSR", "DPZ", "WEN", "JACK",
    # Coca-Cola and sugary drinks
    "KO", "KO.DE", "PEP",
    # Tobacco
    "PM", "MO", "BTI",
    # Big defense
    "LMT", "RTX", "NOC", "GD", "BA", "LDOS", "BWXT",
    # Oil majors
    "XOM", "CVX", "BP", "SHEL", "TTE", "COP", "OXY",
}

MODERATE_CONCERN_INDUSTRIES = {
    "Beverages - Non-Alcoholic",
    "Beverages - Alcoholic",
    "Beverages - Brewers",
    "Beverages - Wineries & Distilleries",
    "Coal",
    "Uranium",
    "Chemicals",
    "Agricultural Inputs",
    "Airlines",
    "Cruise Lines",
}

CLEAN_INDUSTRIES = {
    "Solar",
    "Utilities - Renewable",
    "Semiconductors",
    "Software - Application",
    "Software - Infrastructure",
    "Information Technology Services",
    "Electronic Components",
    "Scientific & Technical Instruments",
    "Specialty Industrial Machinery",  # Only when it's wind/grid
    "Electrical Equipment & Parts",
    "Computer Hardware",
    "Internet Content & Information",
    "Communication Equipment",
}

# ── Watchlists ───────────────────────────────────────────────────────────────
WATCHLIST = {
    "tech": [
        ("SAP SE",             "SAP.DE"),
        ("Infineon",           "IFX.DE"),
        ("ASML",               "ASML.AS"),
        ("Microsoft",          "MSFT"),
        ("NVIDIA",             "NVDA"),
        ("TSMC",               "TSM"),
        ("Cadence Design",     "CDNS"),
        ("Synopsys",           "SNPS"),
        ("AMD",                "AMD"),
    ],
    "renewable": [
        ("Vestas Wind",        "VWS.CO"),
        ("Ørsted",             "ORSTED.CO"),
        ("First Solar",        "FSLR"),
        ("Siemens Energy",     "ENR.DE"),
        ("SMA Solar",          "S92.DE"),
        ("Nordex",             "NDX1.DE"),
        ("Enphase Energy",     "ENPH"),
        ("Array Technologies", "ARRY"),
    ],
    "storage": [
        ("Fluence Energy",     "FLNC"),
        ("Tesla (Megapack)",   "TSLA"),
        ("Enovix",             "ENVX"),
        ("BYD Co.",            "BYDDY"),
        ("Eaton Corp",         "ETN"),
        ("QuantumScape",       "QS"),
    ],
    "ai-infra": [
        ("Vertiv Holdings",    "VRT"),
        ("Arista Networks",    "ANET"),
        ("Equinix",            "EQIX"),
        ("Eaton Corp",         "ETN"),
        ("NVIDIA",             "NVDA"),
        ("AMD",                "AMD"),
    ],
    "india-etf": [
        ("Nippon Nifty 50 ETF",     "NIFTYBEES.NS"),
        ("Mirae Nifty IT ETF",      "ITETF.NS"),
        ("ICICI Pru Nifty IT ETF",  "ITIETF.NS"),
        ("SBI Nifty Next 50 ETF",   "SETFNN50.NS"),
        ("Mirae FANG+ ETF",         "MAFANG.NS"),
        ("HDFC Nifty 50 ETF",       "HDFCNIFTY.NS"),
    ],
}

# ── Personal portfolio ───────────────────────────────────────────────────────
# Updated from portfolio screenshot (01 Jul 2026).
# "dank" still unconfirmed — DBK.DE kept as placeholder.
# ⚠ BA (Boeing) and BP are in AVOID_TICKERS — flagged with ethical 1/5.
PERSONAL_PORTFOLIO = [
    # Core tech — US
    ("BlackBerry",           "BB"),
    ("AMD",                  "AMD"),
    ("NVIDIA",               "NVDA"),
    ("Broadcom",             "AVGO"),
    ("ARM Holdings",         "ARM"),
    ("Astera Labs",          "ALAB"),
    ("Atlassian",            "TEAM"),
    ("Aptiv",                "APTV"),
    ("Alphabet",             "GOOGL"),
    # Core tech — XETRA
    ("ASML",                 "ASML.AS"),
    ("Apple",                "APC.DE"),
    ("Microsoft",            "MSF.DE"),
    ("Adobe",                "ADB.DE"),
    ("Qualcomm",             "QCI.DE"),
    ("Intel",                "INTC"),
    # Automotive / industrial — XETRA
    ("Mercedes-Benz",        "MBG.DE"),
    ("Volkswagen",           "VOW3.DE"),
    ("BMW",                  "BMW3.DE"),
    # Telecoms
    ("Vodafone",             "VOD"),
    # ETFs
    ("iShares MSCI India",   "QDV5.DE"),
    ("Vanguard S&P 500",     "VUSA.DE"),
    ("WisdomTree Renewable", "WRNW"),
    # "dank" placeholder — update once confirmed
    ("Deutsche Bank(?)",     "DBK.DE"),
    # ⚠ ETHICAL FLAGS — below holdings conflict with stated preferences
    ("Boeing",               "BA"),    # ⚠ Defense — ethical 1/5
    ("BP",                   "BP"),    # ⚠ Oil & Gas — ethical 1/5
    ("Greggs",               "GRG.L"), # UK bakery — ethical 2/5
]

# ── Indian MF scheme codes (mfapi.in) — Direct Growth plans
INDIA_MF = [
    (150597, "Mirae Asset AI & Tech ETF FoF",     "Tech / AI",     5),
    (120594, "ICICI Pru Technology Fund",          "Tech",          4),
    (120578, "SBI Technology Opportunities",       "Tech",          4),
    (118537, "Franklin India Technology Fund",     "Tech",          4),
    (135800, "Tata Digital India Fund",            "Tech",          4),
    (119275, "DSP Global Clean Energy FoF",        "Renewable",     5),
    (119028, "DSP Natural Resources & New Energy", "Renewable",     4),
    (148606, "Kotak ESG Exclusionary Fund",        "ESG",           5),
    (148574, "Mirae Asset Nifty 100 ESG FoF",      "ESG",           4),
]

# ── Scoring ──────────────────────────────────────────────────────────────────

def ethical_score(ticker: str, info: dict) -> tuple[int, str]:
    sym = ticker.upper().split(".")[0]
    full_sym = ticker.upper()
    industry = info.get("industry", "") or ""
    sector   = info.get("sector", "") or ""

    if sym in AVOID_TICKERS or full_sym in AVOID_TICKERS:
        return 1, "Explicitly avoided"
    if industry in UNETHICAL_INDUSTRIES:
        return 1, f"Industry: {industry}"
    if sector == "Energy" and "Renewable" not in industry:
        return 1, "Fossil energy"
    if industry in MODERATE_CONCERN_INDUSTRIES:
        return 2, f"Moderate concern: {industry}"
    if industry in CLEAN_INDUSTRIES:
        return 5, "Clean tech / software"
    if sector in ("Technology", "Communication Services"):
        return 4, "Tech sector"
    if sector == "Utilities" and "Renewable" in industry:
        return 5, "Renewable utilities"
    if sector == "Healthcare":
        return 4, "Healthcare"
    if sector == "Industrials" and any(k in (info.get("longName","") or "") for k in ("Wind","Solar","Energy","Grid","Power")):
        return 4, "Clean industrials"
    return 3, f"{sector} / {industry}"


def buying_score(ticker: str, info: dict) -> tuple[int, str]:
    score = 3
    reasons = []

    pe_fwd   = info.get("forwardPE")
    profit_m = info.get("profitMargins") or 0
    rev_g    = info.get("revenueGrowth") or 0
    d_e      = info.get("debtToEquity") or 0
    current  = info.get("currentPrice") or info.get("regularMarketPrice") or 0
    low52    = info.get("fiftyTwoWeekLow") or 0
    high52   = info.get("fiftyTwoWeekHigh") or 0
    pe_trail = info.get("trailingPE")

    # 52W position
    if current and low52 and high52 and high52 > low52:
        pos = (current - low52) / (high52 - low52)
        if pos < 0.30:
            score += 1; reasons.append(f"Near 52W low ({pos*100:.0f}% of range)")
        elif pos > 0.88:
            score -= 1; reasons.append("Near 52W high")

    # P/E
    pe = pe_fwd if pe_fwd and pe_fwd > 0 else pe_trail
    if pe:
        if pe < 15:
            score += 1; reasons.append(f"Low P/E {pe:.1f}x")
        elif pe > 60:
            score -= 1; reasons.append(f"High P/E {pe:.1f}x")

    # Revenue growth
    if rev_g > 0.25:
        score += 1; reasons.append(f"Rev +{rev_g*100:.0f}%")
    elif rev_g < 0:
        score -= 1; reasons.append(f"Rev declining {rev_g*100:.0f}%")

    # Profit margin
    if profit_m > 0.20:
        score += 1; reasons.append(f"Margin {profit_m*100:.0f}%")
    elif profit_m < 0:
        score -= 1; reasons.append("Unprofitable")

    # High debt
    if d_e > 150:
        score -= 1; reasons.append(f"High D/E {d_e:.0f}")

    score = max(1, min(5, score))
    return score, "; ".join(reasons[:3]) or "Neutral"


def stars(n: int, color_on: str = "bright_yellow", color_off: str = "grey50") -> str:
    return f"[{color_on}]{'★' * n}[/{color_on}][{color_off}]{'☆' * (5-n)}[/{color_off}]"


def buy_color(n: int) -> str:
    return {1: "red", 2: "orange3", 3: "yellow3", 4: "chartreuse3", 5: "bright_green"}[n]


def ethics_color(n: int) -> str:
    return {1: "red", 2: "orange3", 3: "yellow3", 4: "cyan", 5: "bright_cyan"}[n]


def fmt_pct(v):
    if v is None: return "[grey50]N/A[/]"
    c = "bright_green" if v > 0 else "red"
    return f"[{c}]{v*100:+.1f}%[/{c}]"


def fmt_price(currency, price):
    if price is None: return "[grey50]N/A[/]"
    return f"[white]{currency} {price:.1f}[/white]"


def fmt_pe(v):
    if v is None or v <= 0: return "[grey50]N/A[/]"
    c = "bright_green" if v < 20 else ("yellow3" if v < 40 else "red")
    return f"[{c}]{v:.1f}x[/{c}]"


def fmt_mcap(v):
    if v is None: return "[grey50]N/A[/]"
    if v >= 1e12: return f"[bold]{v/1e12:.1f}T[/bold]"
    if v >= 1e9:  return f"{v/1e9:.0f}B"
    return f"{v/1e6:.0f}M"

# ── Fetch functions ──────────────────────────────────────────────────────────

def fetch_today_change(ticker_obj) -> float | None:
    try:
        hist = ticker_obj.history(period="2d", interval="1d")
        if len(hist) >= 2:
            return (hist.iloc[-1]["Close"] - hist.iloc[-2]["Close"]) / hist.iloc[-2]["Close"]
    except Exception:
        pass
    return None


def daily_sell_score(info: dict, today_chg: float | None, buy_score: int) -> tuple[int, str]:
    """
    Returns a sell/trim signal score (1–5) and reason.
    5 = strong sell/trim signal, 1 = hold/no signal.
    This is NOT a recommendation to sell — it flags overextension or deteriorating fundamentals.
    """
    score = 0
    reasons = []

    current  = info.get("currentPrice") or info.get("regularMarketPrice") or 0
    low52    = info.get("fiftyTwoWeekLow") or 0
    high52   = info.get("fiftyTwoWeekHigh") or 1
    pe_fwd   = info.get("forwardPE")
    rev_g    = info.get("revenueGrowth") or 0
    profit_m = info.get("profitMargins") or 0

    if current and low52 and high52 > low52:
        pos = (current - low52) / (high52 - low52)
        if pos > 0.92:
            score += 2; reasons.append(f"At {pos*100:.0f}% of 52W range")
        elif pos > 0.80:
            score += 1; reasons.append(f"Near 52W high ({pos*100:.0f}%)")

    if today_chg is not None and today_chg > 0.06:
        score += 2; reasons.append(f"Surged +{today_chg*100:.1f}% today")
    elif today_chg is not None and today_chg > 0.03:
        score += 1; reasons.append(f"Up +{today_chg*100:.1f}% today")

    if pe_fwd and pe_fwd > 80:
        score += 1; reasons.append(f"P/E {pe_fwd:.0f}x — stretched")
    if rev_g < -0.05:
        score += 1; reasons.append(f"Revenue declining {rev_g*100:.1f}%")
    if profit_m < 0:
        score += 1; reasons.append("Unprofitable")

    # Low buy score is itself a sell signal
    if buy_score <= 2:
        score += 1; reasons.append("Weak fundamentals")

    score = max(1, min(5, score))
    return score, "; ".join(reasons[:3]) or "No sell signal"


def sell_color(n: int) -> str:
    return {1: "grey50", 2: "yellow3", 3: "orange3", 4: "red", 5: "bright_red"}[n]


def fetch_stock(name: str, sym: str) -> dict:
    try:
        t = yf.Ticker(sym)
        info = t.info
        if not info or not info.get("symbol"):
            return {"name": name, "ticker": sym, "error": "No data"}
    except Exception as e:
        return {"name": name, "ticker": sym, "error": str(e)[:60]}

    today_chg = fetch_today_change(t)
    bscore, breason = buying_score(sym, info)
    sscore, sreason = daily_sell_score(info, today_chg, bscore)
    escore, ereason = ethical_score(sym, info)
    officers = info.get("companyOfficers", []) or []
    ceo = next((o.get("name","") for o in officers if "CEO" in o.get("title","").upper()),
               officers[0].get("name","") if officers else "N/A")
    return {
        "name": name, "ticker": sym,
        "longName": info.get("longName") or info.get("shortName") or name,
        "country": info.get("country","N/A"),
        "currency": info.get("currency",""),
        "price": info.get("currentPrice") or info.get("regularMarketPrice"),
        "todayChg": today_chg,
        "sector": info.get("sector",""), "industry": info.get("industry",""),
        "marketCap": info.get("marketCap"),
        "pe_fwd": info.get("forwardPE"), "pe_trail": info.get("trailingPE"),
        "profitMargin": info.get("profitMargins"),
        "revenueGrowth": info.get("revenueGrowth"),
        "debtToEquity": info.get("debtToEquity"),
        "roe": info.get("returnOnEquity"),
        "dividendYield": info.get("dividendYield"),
        "52wLow": info.get("fiftyTwoWeekLow"), "52wHigh": info.get("fiftyTwoWeekHigh"),
        "employees": info.get("fullTimeEmployees"), "ceo": ceo,
        "buyScore": bscore, "buyReason": breason,
        "sellScore": sscore, "sellReason": sreason,
        "ethicalScore": escore, "ethicalReason": ereason,
        "summary": (info.get("longBusinessSummary") or "")[:280],
    }


def fetch_india_mf(scheme_code: int, label: str, category: str, eth_score: int) -> dict:
    try:
        r = requests.get(f"http://api.mfapi.in/mf/{scheme_code}", timeout=12)
        data = r.json()
        navs = data.get("data", [])
        meta = data.get("meta", {})
        if not navs:
            return {"name": label, "scheme": scheme_code, "error": "No NAV data"}
        latest_nav = float(navs[0]["nav"])
        nav_date   = navs[0]["date"]
        # 1-year return (approx 252 trading days)
        old_idx = min(252, len(navs)-1)
        try:
            old_nav = float(navs[old_idx]["nav"])
            ret_1y  = (latest_nav - old_nav) / old_nav
        except:
            ret_1y = None
        # 3-month return (approx 63 trading days)
        old3_idx = min(63, len(navs)-1)
        try:
            old3_nav = float(navs[old3_idx]["nav"])
            ret_3m   = (latest_nav - old3_nav) / old3_nav
        except:
            ret_3m = None

        # Simple buy score for MF: based on 1y return + 3m momentum
        bscore = 3
        if ret_1y and ret_1y > 0.30: bscore += 1
        if ret_1y and ret_1y < -0.10: bscore -= 1
        if ret_3m and ret_3m > 0.05: bscore += 1
        if ret_3m and ret_3m < -0.05: bscore -= 1
        bscore = max(1, min(5, bscore))
        breason = f"1Y: {ret_1y*100:+.1f}%" if ret_1y else "N/A"

        return {
            "name": label, "scheme": scheme_code,
            "fullName": meta.get("scheme_name", label),
            "category": category,
            "nav": latest_nav, "navDate": nav_date,
            "ret_1y": ret_1y, "ret_3m": ret_3m,
            "buyScore": bscore, "buyReason": breason,
            "ethicalScore": eth_score,
            "ethicalReason": "ESG/tech/clean" if eth_score >= 4 else "Mixed basket",
        }
    except Exception as e:
        return {"name": label, "scheme": scheme_code, "error": str(e)[:60]}

# ── Rich tables ──────────────────────────────────────────────────────────────

def fmt_today_chg(v):
    if v is None: return "[grey50]—[/]"
    c = "bright_green" if v > 0 else ("red" if v < 0 else "grey50")
    arrow = "▲" if v > 0 else ("▼" if v < 0 else "—")
    return f"[{c}]{arrow} {v*100:+.2f}%[/{c}]"


def make_stock_table(title: str, results: list[dict], show_sell: bool = False) -> Table:
    t = Table(
        title=f"[bold]{title}[/bold]",
        box=box.ROUNDED,
        show_lines=True,
        title_style="bold cyan",
        header_style="bold white on grey23",
        border_style="grey46",
        pad_edge=True,
    )
    t.add_column("Company",    style="bold white",   min_width=18, max_width=22)
    t.add_column("Ticker",     style="cyan",         min_width=10)
    t.add_column("Country",    style="grey74",       min_width=8)
    t.add_column("Price",                            min_width=10, justify="right")
    t.add_column("Today",                            min_width=10, justify="right")
    t.add_column("Fwd P/E",    justify="right",      min_width=7)
    t.add_column("Rev↑",       justify="right",      min_width=7)
    t.add_column("Margin",     justify="right",      min_width=7)
    t.add_column("Buy ★",      justify="center",     min_width=11)
    if show_sell:
        t.add_column("Sell ⚠",  justify="center",    min_width=11)
    t.add_column("Ethics ★",   justify="center",     min_width=11)

    for r in results:
        if "error" in r:
            cols = [r["name"][:22], r["ticker"], "—", "—", "—", "—", "—", "—", "[red]ERROR[/]"]
            if show_sell: cols.append("—")
            cols.append(f"[dim]{r['error'][:20]}[/]")
            t.add_row(*cols)
            continue
        pe = r.get("pe_fwd") or r.get("pe_trail")
        bc  = buy_color(r["buyScore"])
        ec  = ethics_color(r["ethicalScore"])
        row = [
            r["name"][:22],
            f"[cyan]{r['ticker']}[/]",
            r.get("country","")[:12],
            fmt_price(r["currency"], r["price"]),
            fmt_today_chg(r.get("todayChg")),
            fmt_pe(pe),
            fmt_pct(r.get("revenueGrowth")),
            fmt_pct(r.get("profitMargin")),
            f"[{bc}]{stars(r['buyScore'])}[/{bc}]",
        ]
        if show_sell:
            sc = sell_color(r.get("sellScore", 1))
            row.append(f"[{sc}]{stars(r.get('sellScore',1), color_on=sc, color_off='grey30')}[/{sc}]")
        row.append(f"[{ec}]{stars(r['ethicalScore'])}[/{ec}]")
        t.add_row(*row)
    return t


def make_portfolio_table(results: list[dict]) -> Table:
    t = Table(
        title="[bold]Personal Portfolio — Daily Signals[/bold]",
        box=box.ROUNDED,
        show_lines=True,
        title_style="bold bright_yellow",
        header_style="bold white on grey23",
        border_style="bright_yellow",
        pad_edge=True,
    )
    t.add_column("Company",    style="bold white",  min_width=16, max_width=20)
    t.add_column("Ticker",     style="cyan",        min_width=10)
    t.add_column("Price",                           min_width=11, justify="right")
    t.add_column("Today %",                         min_width=10, justify="right")
    t.add_column("52W Pos",                         min_width=8,  justify="right")
    t.add_column("Fwd P/E",    justify="right",     min_width=7)
    t.add_column("Buy ★",      justify="center",    min_width=11)
    t.add_column("Sell ⚠",     justify="center",    min_width=11)
    t.add_column("Ethics ★",   justify="center",    min_width=11)

    for r in results:
        if "error" in r:
            t.add_row(r["name"][:20], r["ticker"], "—","—","—","—","—","—", f"[red]{r['error'][:20]}[/]")
            continue
        pe   = r.get("pe_fwd") or r.get("pe_trail")
        bc   = buy_color(r["buyScore"])
        sc   = sell_color(r.get("sellScore", 1))
        ec   = ethics_color(r["ethicalScore"])

        current = r.get("price") or 0
        low52   = r.get("52wLow") or 0
        high52  = r.get("52wHigh") or 1
        pos     = (current - low52) / max(high52 - low52, 1) * 100 if current else 0
        pos_c   = "bright_green" if pos < 30 else ("red" if pos > 85 else "yellow3")
        pos_str = f"[{pos_c}]{pos:.0f}%[/{pos_c}]"

        t.add_row(
            r["name"][:20],
            f"[cyan]{r['ticker']}[/]",
            fmt_price(r["currency"], r["price"]),
            fmt_today_chg(r.get("todayChg")),
            pos_str,
            fmt_pe(pe),
            f"[{bc}]{stars(r['buyScore'])}[/{bc}]",
            f"[{sc}]{stars(r.get('sellScore',1), color_on=sc, color_off='grey30')}[/{sc}]",
            f"[{ec}]{stars(r['ethicalScore'])}[/{ec}]",
        )
    return t


def make_mf_table(results: list[dict]) -> Table:
    t = Table(
        title="[bold]India — Mutual Funds (Direct Growth)[/bold]",
        box=box.ROUNDED,
        show_lines=True,
        title_style="bold magenta",
        header_style="bold white on grey23",
        border_style="grey46",
        pad_edge=True,
    )
    t.add_column("Fund Name",     style="bold white",  min_width=28, max_width=32)
    t.add_column("Category",      style="grey74",      min_width=10)
    t.add_column("NAV (INR)",     justify="right",     min_width=10)
    t.add_column("1Y Return",     justify="right",     min_width=9)
    t.add_column("3M Return",     justify="right",     min_width=9)
    t.add_column("Buy ★",         justify="center",    min_width=11)
    t.add_column("Ethics ★",      justify="center",    min_width=11)

    for r in results:
        if "error" in r:
            t.add_row(r["name"][:32], "—", "—", "—", "—", "[red]ERROR[/]", f"[dim]{r['error'][:30]}[/]")
            continue
        bc, ec = buy_color(r["buyScore"]), ethics_color(r["ethicalScore"])
        nav_str = f"[white]{r['nav']:.2f}[/white]"
        ret1y_str = fmt_pct(r.get("ret_1y"))
        ret3m_str = fmt_pct(r.get("ret_3m"))
        t.add_row(
            r["name"][:32],
            r["category"],
            nav_str,
            ret1y_str,
            ret3m_str,
            f"[{bc}]{stars(r['buyScore'])}[/{bc}]",
            f"[{ec}]{stars(r['ethicalScore'])}[/{ec}]",
        )
    return t


def make_etf_table(results: list[dict]) -> Table:
    t = Table(
        title="[bold]India — ETFs (NSE)[/bold]",
        box=box.ROUNDED,
        show_lines=True,
        title_style="bold magenta",
        header_style="bold white on grey23",
        border_style="grey46",
        pad_edge=True,
    )
    t.add_column("ETF Name",    style="bold white",  min_width=22)
    t.add_column("Ticker",      style="cyan",        min_width=14)
    t.add_column("Price (INR)", justify="right",     min_width=12)
    t.add_column("52W Low",     justify="right",     min_width=8)
    t.add_column("52W High",    justify="right",     min_width=8)
    t.add_column("Buy ★",       justify="center",    min_width=11)
    t.add_column("Ethics ★",    justify="center",    min_width=11)

    for r in results:
        if "error" in r:
            t.add_row(r["name"], r["ticker"], "—", "—", "—", "[red]ERROR[/]", "—")
            continue
        bc, ec = buy_color(r["buyScore"]), ethics_color(r["ethicalScore"])
        low  = f"{r['52wLow']:.1f}"  if r.get("52wLow")  else "N/A"
        high = f"{r['52wHigh']:.1f}" if r.get("52wHigh") else "N/A"
        t.add_row(
            r["name"][:22],
            f"[cyan]{r['ticker']}[/]",
            fmt_price("INR", r.get("price")),
            f"[grey70]{low}[/]",
            f"[grey70]{high}[/]",
            f"[{bc}]{stars(r['buyScore'])}[/{bc}]",
            f"[{ec}]{stars(r['ethicalScore'])}[/{ec}]",
        )
    return t


def print_verbose(results: list[dict]):
    for r in results:
        if "error" in r or "nav" in r:
            continue
        console.print(Rule(f"[bold cyan]{r['longName']}[/] ([cyan]{r['ticker']}[/])", style="grey46"))
        grid = Table.grid(padding=(0, 2))
        grid.add_column(style="grey74", min_width=14)
        grid.add_column(style="white")
        grid.add_row("CEO",       r.get("ceo","N/A"))
        grid.add_row("Employees", f"{r.get('employees','N/A'):,}" if isinstance(r.get("employees"), int) else "N/A")
        grid.add_row("ROE",       f"{(r.get('roe') or 0)*100:.1f}%")
        grid.add_row("D/E Ratio", f"{r.get('debtToEquity','N/A')}")
        grid.add_row("Dividend",  f"{(r.get('dividendYield') or 0)*100:.1f}%" if r.get("dividendYield") else "None")
        grid.add_row("52W Range", f"{r.get('52wLow','?')} — {r.get('52wHigh','?')}")
        grid.add_row("Buy",       f"[bright_yellow]{r['buyScore']}/5[/] — {r['buyReason']}")
        grid.add_row("Ethics",    f"[bright_cyan]{r['ethicalScore']}/5[/] — {r['ethicalReason']}")
        console.print(grid)
        if r.get("summary"):
            console.print(f"  [dim]{r['summary']}...[/dim]")
        console.print()


def daily_buy_sell_panel(results: list[dict]) -> None:
    """Top 5 buy + Top 5 sell/trim signals across all fetched results for today."""
    valid = [r for r in results if "error" not in r and "nav" not in r]
    if not valid:
        return

    # Buy: ranked by buyScore desc, then by 52W position asc (closer to low = better)
    def buy_rank(r):
        pos = 1.0
        c, lo, hi = r.get("price") or 0, r.get("52wLow") or 0, r.get("52wHigh") or 1
        if c and hi > lo:
            pos = (c - lo) / (hi - lo)
        return (r["buyScore"], -pos, r["ethicalScore"])

    # Sell: ranked by sellScore desc
    def sell_rank(r):
        return (r.get("sellScore", 1), -(r["buyScore"]))

    top_buy  = sorted(valid, key=buy_rank, reverse=True)[:5]
    top_sell = sorted(valid, key=sell_rank, reverse=True)[:5]

    buy_lines = []
    for r in top_buy:
        chg = r.get("todayChg")
        chg_str = f" ([bright_green]{chg*100:+.1f}% today[/])" if chg and chg > 0 else (
                  f" ([red]{chg*100:+.1f}% today[/])" if chg else "")
        buy_lines.append(
            f"  [{buy_color(r['buyScore'])}]{'★'*r['buyScore']}[/]  "
            f"[bold]{r['ticker']:12s}[/]  {r['buyReason']}{chg_str}"
        )

    sell_lines = []
    for r in top_sell:
        chg = r.get("todayChg")
        chg_str = f" ([bright_green]{chg*100:+.1f}% today — extended[/])" if chg and chg > 0.03 else (
                  f" ([red]{chg*100:+.1f}% today[/])" if chg else "")
        sell_lines.append(
            f"  [{sell_color(r.get('sellScore',1))}]{'⚠'*r.get('sellScore',1)}[/]  "
            f"[bold]{r['ticker']:12s}[/]  {r.get('sellReason','')}{chg_str}"
        )

    console.print()
    console.print(Panel(
        f"[bold bright_green]TOP 5 — BUY / ADD TODAY:[/]\n" + "\n".join(buy_lines) +
        f"\n\n[bold bright_red]TOP 5 — SELL / TRIM / AVOID TODAY:[/]\n" + "\n".join(sell_lines) +
        "\n\n[dim italic]Sell signals = overextension or deteriorating fundamentals — not guaranteed direction.[/]",
        title=f"[bold bright_yellow]⚡ Daily Trade Signals — {datetime.now().strftime('%d %b %Y')} ⚡[/]",
        border_style="bright_yellow",
        padding=(1, 3),
    ))


def top_picks_panel(all_results: list[dict]) -> None:
    valid = [r for r in all_results if "error" not in r and "nav" not in r]
    mf_valid = [r for r in all_results if "nav" in r and "error" not in r]

    top_buy    = sorted(valid, key=lambda r: (r["buyScore"], r.get("revenueGrowth") or 0), reverse=True)[:4]
    top_ethics = sorted(valid, key=lambda r: (r["ethicalScore"], r["buyScore"]), reverse=True)[:4]
    top_mf     = sorted(mf_valid, key=lambda r: (r["buyScore"], r.get("ret_1y") or 0), reverse=True)[:3]

    buy_text = "\n".join(
        f"  [{buy_color(r['buyScore'])}]{'★'*r['buyScore']}[/]  [bold]{r['ticker']:14s}[/] {r['buyReason']}"
        for r in top_buy
    )
    eth_text = "\n".join(
        f"  [{ethics_color(r['ethicalScore'])}]{'★'*r['ethicalScore']}[/]  [bold]{r['ticker']:14s}[/] {r['ethicalReason']}"
        for r in top_ethics
    )
    mf_text = "\n".join(
        f"  [{buy_color(r['buyScore'])}]{'★'*r['buyScore']}[/]  [bold]{r['name'][:26]:26s}[/] {r['buyReason']}"
        for r in top_mf
    ) or "  [dim]No MF data[/dim]"

    console.print()
    console.print(Panel(
        f"[bold bright_green]Top Global Buys:[/]\n{buy_text}\n\n"
        f"[bold bright_cyan]Most Ethical (Global):[/]\n{eth_text}\n\n"
        f"[bold magenta]Top Indian MFs:[/]\n{mf_text}",
        title="[bold]★ Summary Picks ★[/bold]",
        border_style="bright_yellow",
        padding=(1, 3),
    ))


def print_avoid_notice():
    console.print()
    console.print(Panel(
        "[red]Screened OUT — Ethical Score 1/5:[/]\n"
        "  [dim]Defense:[/] LMT, RTX, NOC, GD, BA\n"
        "  [dim]Oil & Gas:[/] XOM, CVX, BP, SHEL, TTE, COP\n"
        "  [dim]Fast Food:[/] MCD, YUM, QSR, DPZ\n"
        "  [dim]Tobacco:[/] PM, MO, BTI\n"
        "  [dim]Sugary drinks:[/] KO, PEP",
        title="[bold red]Ethical Exclusions[/bold red]",
        border_style="red",
        padding=(0, 2),
    ))


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Market analysis — sectioned, colored, daily signals")
    parser.add_argument("--tickers", nargs="+")
    parser.add_argument("--section",
                        choices=["all","global","india","portfolio","tech","renewable","storage","ai-infra"],
                        default="all")
    parser.add_argument("--format", choices=["rich","json"], default="rich")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    console.print()
    console.print(Rule(
        f"[bold cyan] Market Analysis  ·  {datetime.now().strftime('%d %b %Y %H:%M')} UTC [/]",
        style="cyan"
    ))

    all_results = []

    # ── PORTFOLIO ────────────────────────────────────────────────────────────
    if args.section == "portfolio" and not args.tickers:
        console.print()
        console.print(Rule("[bold white on grey19]  PERSONAL PORTFOLIO  [/]", style="bright_yellow"))
        console.print(
            "  [dim]⚠  'dank' assumed = DBK.DE (Deutsche Bank). "
            "Run [cyan]--tickers[/] to override.[/dim]\n"
        )
        port_results = []
        for name, sym in PERSONAL_PORTFOLIO:
            console.print(f"  [dim]Fetching {sym}...[/dim]", end="\r")
            port_results.append(fetch_stock(name, sym))
        console.print(" " * 60)
        console.print(make_portfolio_table(port_results))
        all_results.extend(port_results)
        daily_buy_sell_panel(port_results)

        if args.format == "json":
            print(json.dumps(port_results, indent=2, default=str))
            return

        print_avoid_notice()
        console.print(
            "\n  [bold yellow]NOTE:[/] Research tool — not financial advice. "
            "Cross-check news before acting.\n"
            "  [dim]Data: Yahoo Finance[/dim]\n"
        )
        return

    # ── GLOBAL MARKETS ───────────────────────────────────────────────────────
    if args.tickers:
        tickers_to_fetch = [(t, t) for t in args.tickers]
        console.print(Rule("[bold white]Custom Tickers[/]", style="grey46"))
        results = []
        for name, sym in tickers_to_fetch:
            console.print(f"  [dim]Fetching {sym}...[/dim]", end="\r")
            results.append(fetch_stock(name, sym))
        console.print(" " * 50)
        console.print(make_stock_table("Custom Watchlist", results, show_sell=True))
        all_results.extend(results)

    elif args.section in ("all", "global", "tech"):
        console.print()
        console.print(Rule("[bold white on grey19]  GLOBAL MARKETS  [/]", style="white"))
        console.print()

        sections = []
        if args.section in ("all", "global", "tech"):
            sections.append(("tech", "Technology Stocks"))
        if args.section in ("all", "global", "renewable"):
            sections.append(("renewable", "Renewable Energy"))
        if args.section in ("all", "global", "storage"):
            sections.append(("storage", "Energy Storage"))
        if args.section in ("all", "global", "ai-infra"):
            sections.append(("ai-infra", "AI Infrastructure"))

        if args.section == "tech":      sections = [("tech", "Technology Stocks")]
        if args.section == "renewable": sections = [("renewable", "Renewable Energy")]
        if args.section == "storage":   sections = [("storage", "Energy Storage")]
        if args.section == "ai-infra":  sections = [("ai-infra", "AI Infrastructure")]
        if args.section == "global":
            sections = [
                ("tech",      "Technology Stocks"),
                ("renewable", "Renewable Energy"),
                ("storage",   "Energy Storage"),
                ("ai-infra",  "AI Infrastructure"),
            ]

        seen = set()
        for key, title in sections:
            wl = WATCHLIST.get(key, [])
            unique_wl = [(n, s) for n, s in wl if s not in seen]
            seen.update(s for _, s in unique_wl)
            results = []
            for name, sym in unique_wl:
                console.print(f"  [dim]Fetching {sym}...[/dim]", end="\r")
                results.append(fetch_stock(name, sym))
            console.print(" " * 50)
            console.print(make_stock_table(title, results, show_sell=True))
            console.print()
            all_results.extend(results)
            if args.verbose:
                print_verbose(results)

    if args.section in ("all", "india") and not args.tickers:
        console.print()
        console.print(Rule("[bold white on grey19]  INDIAN MARKETS  [/]", style="white"))
        console.print()

        # ETFs
        etf_wl = WATCHLIST["india-etf"]
        etf_results = []
        for name, sym in etf_wl:
            console.print(f"  [dim]Fetching {sym}...[/dim]", end="\r")
            etf_results.append(fetch_stock(name, sym))
        console.print(" " * 50)
        console.print(make_etf_table(etf_results))
        console.print()
        all_results.extend(etf_results)

        # Mutual Funds
        console.print(f"  [dim]Fetching Indian Mutual Fund NAVs...[/dim]", end="\r")
        mf_results = []
        for code, label, category, eth in INDIA_MF:
            mf_results.append(fetch_india_mf(code, label, category, eth))
            time.sleep(0.3)
        console.print(" " * 50)
        console.print(make_mf_table(mf_results))
        all_results.extend(mf_results)

    if args.format == "json":
        print(json.dumps(all_results, indent=2, default=str))
        return

    # Daily signals + summary
    if all_results:
        daily_buy_sell_panel(all_results)
    top_picks_panel(all_results)
    print_avoid_notice()
    console.print()
    console.print(
        "  [bold yellow]NOTE:[/] Research tool — not financial advice. "
        "Cross-check news before acting.\n"
        "  [dim]India MF data: mfapi.in | Stock data: Yahoo Finance[/dim]\n"
    )


if __name__ == "__main__":
    main()
