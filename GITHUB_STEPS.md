# Publishing on GitHub (step by step)

## 1. Create the repository
1. Go to https://github.com/new (signed in as JimBolanos).
2. Repository name: `brickcode-studio`. Public. Do NOT add a README, .gitignore or license (they are already here).
3. Click **Create repository**.

## 2. Upload the files from your computer
Unzip `brickcode_studio.zip`, then in a terminal:
```
cd brickcode_studio_pkg
git init -b main
git add .
git commit -m "BrickCode Studio 2.0"
git remote add origin https://github.com/JimBolanos/brickcode-studio.git
git push -u origin main
```
GitHub asks for a login: use a Personal Access Token as the password
(GitHub → Settings → Developer settings → Personal access tokens), or run `gh auth login` first if you have the GitHub CLI.

## 3. Make the first release
1. Repo page → **Releases** → **Create a new release**.
2. Tag: `v2.0.0`. Title: `BrickCode Studio 2.0`.
3. Describe it in a few lines (NQC + NXC, Linux and macOS, English/Spanish).
4. Attach `brickcode_studio.zip` and click **Publish release**.

## 4. Finish the page
- Edit the repo description (gear next to "About"): "Editor for classic robot bricks: NQC (RCX, Scout) and NXC (NXT). Linux and macOS."
- Add topics: `nqc`, `nxc`, `rcx`, `nxt`, `mindstorms`, `gtk4`, `robotics`.
- Share the link in your blog.
