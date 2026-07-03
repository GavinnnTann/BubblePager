-- TelePager Supabase schema. Run in the Supabase SQL editor (or via psql).

-- ── Storage bucket for transcoded .mjpeg files (private) ────────────────────
insert into storage.buckets (id, name, public)
values ('videos', 'videos', false)
on conflict (id) do nothing;

-- ── Metadata / history table ────────────────────────────────────────────────
create table if not exists public.videos (
    id            text primary key,          -- Telegram file_unique_id
    file_id       text,                       -- Telegram file_id (re-fetch)
    sender_name   text,
    sender_id     bigint,
    chat_id       bigint,
    frames        integer,
    duration_sec  real,
    bytes         integer,
    storage_path  text,                       -- '<id>.mjpeg' in the videos bucket
    created_at    timestamptz not null default now()
);

create index if not exists videos_created_at_idx
    on public.videos (created_at desc);

-- RLS: the server uses the service-role key, which bypasses RLS. Enable RLS so
-- the anon/public key can't read the table; add read policies later if a
-- companion app needs them.
alter table public.videos enable row level security;
