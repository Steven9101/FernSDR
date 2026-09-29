<script lang="ts">
  interface Props {
    label: string;
    checked: boolean;
    onChange: (checked: boolean) => void;
    title?: string;
  }

  let { label, checked, onChange, title }: Props = $props();
  const id = $props.id();
</script>

<div class="field" {title}>
  <label class="field__label" for={id}>
    {label}
  </label>
  <div class="field__control field__control--end">
  <button
    {id}
    type="button"
    role="switch"
    aria-checked={checked}
    aria-label={label}
    class="switch{checked ? ' is-on' : ''}"
    onclick={() => onChange(!checked)}
  >
    <span class="switch__thumb"></span>
  </button>
  </div>
</div>
