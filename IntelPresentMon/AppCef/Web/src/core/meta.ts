// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: MIT
export type Distinct<T, DistinctName> = T & { __TYPE__: DistinctName };

type StandardEnum<T> = {
    [id: string]: T | string;
    [nu: number]: string;
}

export function getEnumValues<T extends StandardEnum<number>>(e: T): number[] {
    return Object.keys(e).filter(k => typeof e[k] !== "number").map(k => parseInt(k));
}