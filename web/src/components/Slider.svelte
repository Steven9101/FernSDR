<script lang="ts">
  interface Props {
    label: string;
    min: number;
    max: number;
    step?: number;
    value: number;
    onChange: (value: number) => void;
    format?: (value: number) => string;
    title?: string;
  }

  let { label, min, max, step = 1, value, onChange, format, title }: Props = $props();
  const id = $props.id();
</script>

<div class="field" {title}>
  <label class="field__label" for={id}>
    {label}
  </label>
  <div class="field__control">
    <input
      {id}
      class="slider"
      type="range"
      {min}
      {max}
      {step}
      {value}
      oninput={(event) => onChange(Number(event.currentTarget.value))}
    />
  </div>
  <span class="field__value">{format ? format(value) : value}</span>
</div>
