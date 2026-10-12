-- Appearance: Everforest dark/hard, using foot's background.
vim.opt.termguicolors = true
vim.opt.background = "dark"

vim.g.everforest_background = "hard"
vim.g.everforest_transparent_background = 2

vim.cmd("packadd everforest")
vim.cmd.colorscheme("everforest")

-- Disable providers that this configuration does not use.
vim.g.loaded_node_provider = 0
vim.g.loaded_perl_provider = 0
vim.g.loaded_python3_provider = 0
vim.g.loaded_ruby_provider = 0

-- Simple editor defaults.
vim.opt.number = true
vim.opt.wrap = false
vim.opt.scrolloff = 5
vim.opt.signcolumn = "yes"

vim.opt.ignorecase = true
vim.opt.smartcase = true

vim.opt.splitright = true
vim.opt.splitbelow = true

-- An initial indentation preference; easy to change later.
vim.opt.expandtab = true
vim.opt.tabstop = 4
vim.opt.shiftwidth = 4

-- Use the Wayland clipboard for normal copying and pasting.
vim.opt.clipboard = "unnamedplus"

-- Built-in filename completion for :find.
vim.opt.path:append("**")
vim.opt.wildmenu = true
vim.opt.wildignore:append({ "*.o", "*.a", "*.so", ".git/**" })

-- Space is the prefix for our few custom shortcuts.
vim.g.mapleader = " "

vim.keymap.set("n", "<leader>w", "<cmd>write<CR>",
    { desc = "Save file" })

vim.keymap.set("n", "<leader>e", "<cmd>Explore<CR>",
    { desc = "Browse files" })

vim.keymap.set("n", "<leader>/", "<cmd>nohlsearch<CR>",
    { desc = "Clear search highlighting" })

-- Keep diagnostics quiet: underline issues, but no inline messages.
vim.diagnostic.config({
    virtual_text = false,
    virtual_lines = false,
    underline = true,
    signs = true,
    update_in_insert = false,
    severity_sort = true,
})

-- C and C++ language support, using Neovim's built-in LSP client.
vim.lsp.config("clangd", {
    cmd = { "clangd", "--background-index" },
    filetypes = { "c", "cpp" },
    root_markers = { "compile_commands.json", "compile_flags.txt", ".git" },
})

vim.opt.completeopt = { "menu", "menuone", "noselect", "popup" }

vim.api.nvim_create_autocmd("LspAttach", {
    callback = function(event)
        local client = vim.lsp.get_client_by_id(event.data.client_id)
        if not client then
            return
        end

        if client:supports_method("textDocument/completion") then
            vim.lsp.completion.enable(true, client.id, event.buf, {
                autotrigger = true,
            })

            vim.keymap.set("i", "<C-j>", vim.lsp.completion.get, {
                buffer = event.buf,
                desc = "Request completion",
            })
        end

        local opts = { buffer = event.buf }

        vim.keymap.set("n", "gd", vim.lsp.buf.definition, opts)
        vim.keymap.set("n", "<leader>d", vim.diagnostic.open_float, opts)

        vim.keymap.set("n", "<leader>f", function()
            vim.lsp.buf.format({ async = true })
        end, opts)
    end,
})

vim.lsp.enable("clangd")

-- Build individual C/C++ files with GCC.
vim.api.nvim_create_autocmd("FileType", {
    pattern = { "c", "cpp" },
    callback = function(event)
        -- Teach Neovim how to read GCC's errors and warnings.
        vim.cmd("compiler gcc")

        local function build()
            local source = vim.api.nvim_buf_get_name(event.buf)

            if source == "" then
                vim.notify("Save the file with a name first.", vim.log.levels.WARN)
                return
            end

            vim.cmd("write")

            -- /path/main.c becomes /path/main.
            local output = vim.fn.fnamemodify(source, ":r")
            local is_cpp = vim.bo[event.buf].filetype == "cpp"

            local compiler = is_cpp and "g++" or "gcc"
            local standard = is_cpp and "-std=c++17" or "-std=c17"

            vim.bo[event.buf].makeprg = table.concat({
                compiler,
                standard,
                "-Wall -Wextra -Wpedantic -g",
                vim.fn.shellescape(source),
                "-o",
                vim.fn.shellescape(output),
            }, " ")

            vim.cmd("make")
            vim.cmd("cwindow")
        end

        vim.keymap.set("n", "<leader>b", build, {
            buffer = event.buf,
            desc = "Save and build current file",
        })
    end,
})

-- Preserve undo history between editing sessions.
local undo_dir = vim.fn.stdpath("state") .. "/undo"
vim.fn.mkdir(undo_dir, "p", 448) -- Owner-only directory permissions.

vim.opt.undodir = undo_dir
vim.opt.undofile = true

vim.keymap.set("n", "<leader>h", function()
    local file = vim.fn.stdpath("config") .. "/shortcuts.md"
    vim.cmd("vsplit " .. vim.fn.fnameescape(file))
end, { desc = "Open shortcut reference" })
